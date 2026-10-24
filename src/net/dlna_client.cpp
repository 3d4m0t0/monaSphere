#include "net/dlna_client.hpp"

#include "common.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <map>
#include <sstream>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

namespace vrp::dlna {
namespace {

std::string trim(std::string s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
  return s;
}

std::string to_lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string xml_unescape_once(std::string s) {
  auto repl = [&](const char* from, const char* to) {
    const size_t fl = std::strlen(from);
    const size_t tl = std::strlen(to);
    size_t p = 0;
    while ((p = s.find(from, p)) != std::string::npos) {
      s.replace(p, fl, to);
      p += tl;
    }
  };
  // &amp; last so double-escaped &amp;lt; becomes &lt; then < on next pass.
  repl("&lt;", "<");
  repl("&gt;", ">");
  repl("&quot;", "\"");
  repl("&apos;", "'");
  repl("&#39;", "'");
  repl("&amp;", "&");
  return s;
}

std::string decode_numeric_entities(std::string s) {
  size_t p = 0;
  while ((p = s.find("&#", p)) != std::string::npos) {
    const size_t end = s.find(';', p);
    if (end == std::string::npos || end - p > 12) {
      p += 2;
      continue;
    }
    unsigned long cp = 0;
    if (s[p + 2] == 'x' || s[p + 2] == 'X') {
      cp = std::strtoul(s.c_str() + p + 3, nullptr, 16);
    } else {
      cp = std::strtoul(s.c_str() + p + 2, nullptr, 10);
    }
    std::string utf8;
    if (cp < 0x80) {
      utf8.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      utf8.push_back(static_cast<char>(0xC0 | (cp >> 6)));
      utf8.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      utf8.push_back(static_cast<char>(0xE0 | (cp >> 12)));
      utf8.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      utf8.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x110000) {
      utf8.push_back(static_cast<char>(0xF0 | (cp >> 18)));
      utf8.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      utf8.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      utf8.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    if (utf8.empty()) {
      p = end + 1;
      continue;
    }
    s.replace(p, end - p + 1, utf8);
    p += utf8.size();
  }
  return s;
}

/** SOAP Result is often double-escaped; keep unescaping until stable (cap 4). */
std::string xml_unescape(std::string s) {
  for (int i = 0; i < 4; ++i) {
    std::string next = xml_unescape_once(s);
    if (next == s) break;
    s = std::move(next);
  }
  return decode_numeric_entities(std::move(s));
}

bool parse_url(const std::string& url, std::string& host, int& port, std::string& path, bool& https) {
  https = false;
  host.clear();
  path = "/";
  port = 80;
  std::string u = url;
  if (u.rfind("https://", 0) == 0) {
    https = true;
    port = 443;
    u = u.substr(8);
  } else if (u.rfind("http://", 0) == 0) {
    u = u.substr(7);
  } else {
    return false;
  }
  const size_t slash = u.find('/');
  std::string hostport = slash == std::string::npos ? u : u.substr(0, slash);
  path = slash == std::string::npos ? "/" : u.substr(slash);
  const size_t colon = hostport.rfind(':');
  if (colon != std::string::npos && hostport.find(']') == std::string::npos) {
    host = hostport.substr(0, colon);
    port = std::atoi(hostport.substr(colon + 1).c_str());
    if (port <= 0) return false;
  } else {
    host = hostport;
  }
  return !host.empty();
}

std::string join_url(const std::string& base, const std::string& ref) {
  if (ref.rfind("http://", 0) == 0 || ref.rfind("https://", 0) == 0) return ref;
  std::string b = base;
  if (b.empty()) return ref;
  if (ref.empty()) return b;
  if (ref[0] == '/') {
    std::string host, path;
    int port = 80;
    bool https = false;
    if (!parse_url(b, host, port, path, https)) return ref;
    std::ostringstream o;
    o << (https ? "https://" : "http://") << host;
    if ((!https && port != 80) || (https && port != 443)) o << ':' << port;
    o << ref;
    return o.str();
  }
  if (b.back() != '/') {
    const size_t slash = b.rfind('/');
    if (slash != std::string::npos && slash > 7) b = b.substr(0, slash + 1);
    else b.push_back('/');
  }
  return b + ref;
}

bool http_exchange(const std::string& method, const std::string& url, const std::string& body,
                   const std::vector<std::pair<std::string, std::string>>& headers, int timeout_ms,
                   std::string& out_body, int& out_status) {
  out_body.clear();
  out_status = 0;
  std::string host, path;
  int port = 80;
  bool https = false;
  if (!parse_url(url, host, port, path, https)) return false;
  if (https) {
    VRP_LOG("dlna: https not supported (%s)", url.c_str());
    return false;
  }

  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* res = nullptr;
  const std::string port_s = std::to_string(port);
  if (getaddrinfo(host.c_str(), port_s.c_str(), &hints, &res) != 0 || !res) return false;

  int fd = -1;
  for (addrinfo* ai = res; ai; ai = ai->ai_next) {
    fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0) continue;
    const int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    const int cr = ::connect(fd, ai->ai_addr, ai->ai_addrlen);
    if (cr == 0) break;
    if (errno == EINPROGRESS) {
      fd_set wfds;
      FD_ZERO(&wfds);
      FD_SET(fd, &wfds);
      timeval tv{};
      tv.tv_sec = timeout_ms / 1000;
      tv.tv_usec = (timeout_ms % 1000) * 1000;
      if (select(fd + 1, nullptr, &wfds, nullptr, &tv) > 0) {
        int so_err = 0;
        socklen_t sl = sizeof(so_err);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_err, &sl);
        if (so_err == 0) break;
      }
    }
    ::close(fd);
    fd = -1;
  }
  freeaddrinfo(res);
  if (fd < 0) return false;
  fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) & ~O_NONBLOCK);

  timeval tv{};
  tv.tv_sec = timeout_ms / 1000;
  tv.tv_usec = (timeout_ms % 1000) * 1000;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

  std::ostringstream req;
  req << method << ' ' << path << " HTTP/1.1\r\n";
  req << "Host: " << host;
  if (port != 80) req << ':' << port;
  req << "\r\n";
  req << "Connection: close\r\n";
  req << "User-Agent: monaSphere/1.0\r\n";
  for (const auto& h : headers) req << h.first << ": " << h.second << "\r\n";
  if (!body.empty()) req << "Content-Length: " << body.size() << "\r\n";
  req << "\r\n";
  if (!body.empty()) req << body;
  const std::string req_s = req.str();
  size_t sent = 0;
  while (sent < req_s.size()) {
    const ssize_t n = ::send(fd, req_s.data() + sent, req_s.size() - sent, 0);
    if (n <= 0) {
      ::close(fd);
      return false;
    }
    sent += static_cast<size_t>(n);
  }

  std::string raw;
  char buf[4096];
  for (;;) {
    const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
    if (n <= 0) break;
    raw.append(buf, static_cast<size_t>(n));
    if (raw.size() > 8 * 1024 * 1024) break;
  }
  ::close(fd);

  const size_t hdr_end = raw.find("\r\n\r\n");
  if (hdr_end == std::string::npos) return false;
  const std::string hdr = raw.substr(0, hdr_end);
  out_body = raw.substr(hdr_end + 4);
  if (hdr.size() >= 12 && hdr.rfind("HTTP/", 0) == 0) {
    out_status = std::atoi(hdr.c_str() + 9);
  }
  // Chunked: rare on UPnP devices; skip if present and body looks like chunks.
  const std::string hdr_l = to_lower(hdr);
  if (hdr_l.find("transfer-encoding: chunked") != std::string::npos) {
    std::string decoded;
    size_t p = 0;
    while (p < out_body.size()) {
      const size_t line = out_body.find("\r\n", p);
      if (line == std::string::npos) break;
      const int sz = static_cast<int>(std::strtol(out_body.c_str() + p, nullptr, 16));
      p = line + 2;
      if (sz <= 0) break;
      if (p + static_cast<size_t>(sz) > out_body.size()) break;
      decoded.append(out_body, p, static_cast<size_t>(sz));
      p += static_cast<size_t>(sz) + 2;
    }
    out_body = std::move(decoded);
  }
  return out_status >= 200 && out_status < 300;
}

std::string xml_attr(const std::string& tag, const char* name) {
  const std::string tl = to_lower(tag);
  const std::string key = to_lower(std::string(name)) + "=\"";
  size_t p = tl.find(key);
  char quote = '"';
  if (p == std::string::npos) {
    const std::string key2 = to_lower(std::string(name)) + "='";
    p = tl.find(key2);
    if (p == std::string::npos) return {};
    quote = '\'';
    p += key2.size();
  } else {
    p += key.size();
  }
  const size_t e = tag.find(quote, p);
  if (e == std::string::npos) return {};
  return tag.substr(p, e - p);
}

/** Local or namespaced element text: <title>, <dc:title>, <ns:title …>. */
std::string xml_tag_text(const std::string& xml, const char* tag) {
  const std::string want = to_lower(tag);
  size_t p = 0;
  while (p < xml.size()) {
    const size_t lt = xml.find('<', p);
    if (lt == std::string::npos) break;
    if (lt + 1 < xml.size() && (xml[lt + 1] == '/' || xml[lt + 1] == '!' || xml[lt + 1] == '?')) {
      p = lt + 1;
      continue;
    }
    size_t name_start = lt + 1;
    size_t name_end = name_start;
    while (name_end < xml.size()) {
      const char c = xml[name_end];
      if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '>' || c == '/') break;
      ++name_end;
    }
    std::string ename = to_lower(xml.substr(name_start, name_end - name_start));
    const size_t colon = ename.rfind(':');
    const std::string local = colon == std::string::npos ? ename : ename.substr(colon + 1);
    const size_t gt = xml.find('>', name_end);
    if (gt == std::string::npos) break;
    if (local != want) {
      p = gt + 1;
      continue;
    }
    if (xml[gt - 1] == '/') {
      p = gt + 1;
      continue;
    }
    const std::string close = std::string("</") + xml.substr(name_start, name_end - name_start) + ">";
    // Case-insensitive close search
    std::string close_l = to_lower(close);
    std::string rest_l = to_lower(xml.substr(gt + 1));
    const size_t c_rel = rest_l.find(close_l);
    if (c_rel == std::string::npos) {
      p = gt + 1;
      continue;
    }
    return xml.substr(gt + 1, c_rel);
  }
  return {};
}

std::string strip_cdata(std::string s) {
  s = trim(s);
  if (s.rfind("<![CDATA[", 0) == 0) {
    s = s.substr(9);
    const size_t e = s.rfind("]]>");
    if (e != std::string::npos) s = s.substr(0, e);
  }
  return s;
}

std::string extract_soap_result(const std::string& soap) {
  const std::string low = to_lower(soap);
  size_t p = low.find(":result");
  if (p == std::string::npos) p = low.find("<result");
  if (p == std::string::npos) return {};
  const size_t gt = soap.find('>', p);
  if (gt == std::string::npos) return {};
  size_t close = low.find(":result>", gt);
  if (close == std::string::npos) close = low.find("</result>", gt);
  if (close == std::string::npos) return {};
  // Walk back to '<' of closing tag
  size_t c0 = close;
  while (c0 > gt && soap[c0] != '<') --c0;
  if (c0 <= gt) return {};
  return soap.substr(gt + 1, c0 - (gt + 1));
}

std::string pick_display_title(const std::string& block, const std::string& id) {
  const char* keys[] = {"title", "programmedTitle", "programTitle", "longDescription", "description",
                        "name"};
  std::string best;
  for (const char* k : keys) {
    std::string t = trim(xml_unescape(strip_cdata(xml_tag_text(block, k))));
    if (t.empty()) continue;
    // Prefer human-readable over bare object ids / .dtp file names when better exists.
    const std::string tl = to_lower(t);
    const bool looks_file = tl.size() > 4 && (tl.rfind(".dtp") == tl.size() - 4 ||
                                              tl.rfind(".tts") == tl.size() - 4 ||
                                              tl.rfind(".m2ts") == tl.size() - 5);
    const bool looks_id = (t == id) || (t.find_first_not_of("0123456789._-") == std::string::npos);
    if (best.empty()) best = t;
    if (!looks_id && !looks_file) return t;
    if (!looks_file && best.size() < t.size()) best = t;
  }
  if (!best.empty()) return best;
  return id.empty() ? std::string("(無題)") : id;
}

bool protocol_is_dtcp(const std::string& protocol, const std::string& mime, const std::string& href) {
  const std::string p = to_lower(protocol);
  const std::string m = to_lower(mime);
  const std::string h = to_lower(href);
  if (p.find("dtcp") != std::string::npos) return true;
  if (m.find("dtcp") != std::string::npos) return true;
  if (m.find("application/x-dtcp") != std::string::npos) return true;
  if (h.size() > 4 && h.rfind(".dtp") == h.size() - 4) return true;
  return false;
}

int mime_score(const std::string& mime, const std::string& protocol, bool dtcp) {
  if (dtcp) return -500;  // never prefer DTCP for FFmpeg playback
  const std::string m = to_lower(mime);
  const std::string p = to_lower(protocol);
  int s = 0;
  if (m.find("video/") == 0) s += 100;
  if (m.find("mpeg") != std::string::npos || m.find("mp2t") != std::string::npos ||
      m.find("vnd.dlna.mpeg-tts") != std::string::npos)
    s += 20;
  if (m.find("mp4") != std::string::npos || m.find("mkv") != std::string::npos) s += 15;
  if (p.find("http-get") != std::string::npos) s += 10;
  if (m.find("image/") == 0 || m.find("audio/") == 0) s -= 50;
  if (m.find("application/") == 0) s -= 80;
  return s;
}

struct ResPick {
  std::string url;
  std::string mime;
  bool any_dtcp = false;
  bool has_clear = false;
};

ResPick pick_best_res(const std::string& item_xml) {
  ResPick out;
  int best = -1000;
  size_t p = 0;
  const std::string low = to_lower(item_xml);
  while ((p = low.find("<res", p)) != std::string::npos) {
    const size_t gt = item_xml.find('>', p);
    if (gt == std::string::npos) break;
    const std::string open = item_xml.substr(p, gt - p + 1);
    if (open.size() >= 2 && open[open.size() - 2] == '/') {
      p = gt + 1;
      continue;
    }
    size_t close = low.find("</res>", gt);
    if (close == std::string::npos) break;
    std::string href = trim(item_xml.substr(gt + 1, close - (gt + 1)));
    href = xml_unescape(href);
    const std::string proto = xml_attr(open, "protocolInfo");
    std::string m;
    {
      size_t a = proto.find(':');
      size_t b = a == std::string::npos ? std::string::npos : proto.find(':', a + 1);
      size_t c = b == std::string::npos ? std::string::npos : proto.find(':', b + 1);
      if (b != std::string::npos && c != std::string::npos) m = proto.substr(b + 1, c - (b + 1));
    }
    const bool dtcp = protocol_is_dtcp(proto, m, href);
    if (dtcp) out.any_dtcp = true;
    const int sc = mime_score(m, proto, dtcp);
    if (sc > best && (href.rfind("http://", 0) == 0 || href.rfind("https://", 0) == 0)) {
      best = sc;
      out.url = href;
      out.mime = m;
      out.has_clear = !dtcp;
    }
    p = close + 6;
  }
  if (!out.has_clear) {
    out.url.clear();
    out.mime.clear();
  }
  return out;
}

bool parse_device_description(const std::string& xml, const std::string& base_hint, Server& out) {
  out.friendly_name = trim(xml_unescape(strip_cdata(xml_tag_text(xml, "friendlyName"))));
  if (out.friendly_name.empty()) out.friendly_name = "DLNA Server";

  // Prefer ContentDirectory service controlURL.
  size_t p = 0;
  std::string control;
  while ((p = xml.find("<service", p)) != std::string::npos) {
    const size_t end = xml.find("</service>", p);
    if (end == std::string::npos) break;
    const std::string block = xml.substr(p, end - p);
    const std::string stype = xml_tag_text(block, "serviceType");
    if (stype.find("ContentDirectory") != std::string::npos) {
      control = trim(xml_tag_text(block, "controlURL"));
      break;
    }
    p = end + 10;
  }
  if (control.empty()) return false;

  std::string base = base_hint;
  const std::string bu = trim(xml_tag_text(xml, "URLBase"));
  if (!bu.empty()) base = bu;
  if (base.empty()) return false;
  if (base.back() != '/' && control.rfind('/') != 0) {
    // keep
  }
  out.base_url = base;
  out.control_url = join_url(base, control);
  return !out.control_url.empty();
}

std::vector<Item> parse_didl(const std::string& didl) {
  std::vector<Item> items;
  auto parse_block = [&](const char* tag, bool container) {
    const std::string want = to_lower(tag);
    size_t p = 0;
    while (p < didl.size()) {
      const size_t lt = didl.find('<', p);
      if (lt == std::string::npos) break;
      if (lt + 1 >= didl.size() || didl[lt + 1] == '/' || didl[lt + 1] == '!' || didl[lt + 1] == '?') {
        p = lt + 1;
        continue;
      }
      size_t name_start = lt + 1;
      size_t name_end = name_start;
      while (name_end < didl.size()) {
        const char c = didl[name_end];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '>' || c == '/') break;
        ++name_end;
      }
      std::string ename = to_lower(didl.substr(name_start, name_end - name_start));
      const size_t colon = ename.rfind(':');
      const std::string local = colon == std::string::npos ? ename : ename.substr(colon + 1);
      const size_t gt = didl.find('>', name_end);
      if (gt == std::string::npos) break;
      if (local != want) {
        p = gt + 1;
        continue;
      }
      if (didl[gt - 1] == '/') {
        p = gt + 1;
        continue;
      }
      const std::string close_exact = std::string("</") + didl.substr(name_start, name_end - name_start) + ">";
      const std::string close_l = to_lower(close_exact);
      const std::string rest_l = to_lower(didl.substr(gt + 1));
      const size_t c_rel = rest_l.find(close_l);
      if (c_rel == std::string::npos) {
        p = gt + 1;
        continue;
      }
      const size_t c = gt + 1 + c_rel;
      const std::string open_tag = didl.substr(lt, gt - lt + 1);
      const std::string block = didl.substr(lt, c + close_exact.size() - lt);
      Item it;
      it.is_container = container;
      it.id = xml_attr(open_tag, "id");
      if (it.id.empty()) it.id = xml_attr(open_tag, "ID");
      it.parent_id = xml_attr(open_tag, "parentID");
      if (it.parent_id.empty()) it.parent_id = xml_attr(open_tag, "parentId");
      it.title = pick_display_title(block, it.id);
      if (!container) {
        const ResPick res = pick_best_res(block);
        it.dtcp_protected = res.any_dtcp;
        it.url = res.url;
        it.mime = res.mime;
        it.playable = !it.url.empty() && !it.dtcp_protected;
        if (it.dtcp_protected && it.url.empty()) {
          // Keep a hint in the title when REGZA exposes only .dtp / DTCP-IP.
          if (it.title.find("DTCP") == std::string::npos && it.title.find("保護") == std::string::npos) {
            it.title += " [DTCP保護・再生不可]";
          }
        }
      } else {
        it.playable = false;
      }
      items.push_back(std::move(it));
      p = c + close_exact.size();
    }
  };
  parse_block("container", true);
  parse_block("item", false);
  return items;
}

}  // namespace

std::vector<Server> discover(int timeout_ms) {
  std::vector<Server> servers;
  std::map<std::string, Server> by_usn;

  const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) return servers;

  int yes = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
  timeval tv{};
  tv.tv_sec = 0;
  tv.tv_usec = 200 * 1000;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  sockaddr_in local{};
  local.sin_family = AF_INET;
  local.sin_port = htons(0);
  local.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(fd, reinterpret_cast<sockaddr*>(&local), sizeof(local)) < 0) {
    ::close(fd);
    return servers;
  }

  const char* msg =
      "M-SEARCH * HTTP/1.1\r\n"
      "HOST: 239.255.255.250:1900\r\n"
      "MAN: \"ssdp:discover\"\r\n"
      "MX: 2\r\n"
      "ST: urn:schemas-upnp-org:device:MediaServer:1\r\n"
      "\r\n";

  sockaddr_in dest{};
  dest.sin_family = AF_INET;
  dest.sin_port = htons(1900);
  inet_pton(AF_INET, "239.255.255.250", &dest.sin_addr);
  ::sendto(fd, msg, std::strlen(msg), 0, reinterpret_cast<sockaddr*>(&dest), sizeof(dest));
  // Also search rootdevice — some recorders answer only this.
  const char* msg2 =
      "M-SEARCH * HTTP/1.1\r\n"
      "HOST: 239.255.255.250:1900\r\n"
      "MAN: \"ssdp:discover\"\r\n"
      "MX: 2\r\n"
      "ST: upnp:rootdevice\r\n"
      "\r\n";
  ::sendto(fd, msg2, std::strlen(msg2), 0, reinterpret_cast<sockaddr*>(&dest), sizeof(dest));

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  char buf[8192];
  while (std::chrono::steady_clock::now() < deadline) {
    sockaddr_in from{};
    socklen_t fl = sizeof(from);
    const ssize_t n = ::recvfrom(fd, buf, sizeof(buf) - 1, 0, reinterpret_cast<sockaddr*>(&from), &fl);
    if (n <= 0) continue;
    buf[n] = 0;
    const std::string resp(buf, static_cast<size_t>(n));
    const std::string resp_l = to_lower(resp);
    if (resp_l.find("mediaserver") == std::string::npos &&
        resp_l.find("contentdirectory") == std::string::npos) {
      // rootdevice replies without MediaServer in ST — still try LOCATION.
      if (resp_l.find("location:") == std::string::npos) continue;
    }
    std::string location;
    std::string usn;
    std::istringstream iss(resp);
    std::string line;
    while (std::getline(iss, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      const std::string ll = to_lower(line);
      if (ll.rfind("location:", 0) == 0) location = trim(line.substr(9));
      if (ll.rfind("usn:", 0) == 0) usn = trim(line.substr(4));
    }
    if (location.empty()) continue;
    if (usn.empty()) usn = location;
    if (by_usn.count(usn)) continue;

    std::string body;
    int status = 0;
    if (!http_exchange("GET", location, {}, {}, 4000, body, status)) continue;

    Server s;
    s.usn = usn;
    // Base from location directory.
    std::string base = location;
    const size_t slash = base.rfind('/');
    if (slash != std::string::npos && slash > 8) base = base.substr(0, slash + 1);
    if (!parse_device_description(body, base, s)) continue;
    // Filter non-MediaServer rootdevice noise: must have ContentDirectory (already required).
    by_usn.emplace(usn, std::move(s));
  }
  ::close(fd);

  servers.reserve(by_usn.size());
  for (auto& kv : by_usn) servers.push_back(std::move(kv.second));
  std::sort(servers.begin(), servers.end(),
            [](const Server& a, const Server& b) { return a.friendly_name < b.friendly_name; });
  VRP_LOG("dlna: discovered %zu MediaServer(s)", servers.size());
  return servers;
}

std::vector<Item> browse(const Server& server, const std::string& object_id, int timeout_ms,
                         int starting_index, int requested_count) {
  if (server.control_url.empty()) return {};
  const std::string oid = object_id.empty() ? "0" : object_id;
  std::ostringstream body;
  body << "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
          "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
          "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
          "<s:Body>"
          "<u:Browse xmlns:u=\"urn:schemas-upnp-org:service:ContentDirectory:1\">"
          "<ObjectID>"
       << oid
       << "</ObjectID>"
          "<BrowseFlag>BrowseDirectChildren</BrowseFlag>"
          "<Filter>*</Filter>"
          "<StartingIndex>"
       << starting_index
       << "</StartingIndex>"
          "<RequestedCount>"
       << requested_count
       << "</RequestedCount>"
          "<SortCriteria></SortCriteria>"
          "</u:Browse>"
          "</s:Body>"
          "</s:Envelope>";

  std::vector<std::pair<std::string, std::string>> hdrs = {
      {"Content-Type", "text/xml; charset=\"utf-8\""},
      {"SOAPACTION", "\"urn:schemas-upnp-org:service:ContentDirectory:1#Browse\""},
  };
  std::string resp;
  int status = 0;
  if (!http_exchange("POST", server.control_url, body.str(), hdrs, timeout_ms, resp, status)) {
    VRP_LOG("dlna: Browse failed (%s id=%s status=%d)", server.friendly_name.c_str(), oid.c_str(),
            status);
    return {};
  }

  std::string result = extract_soap_result(resp);
  if (result.empty()) result = xml_tag_text(resp, "Result");
  result = strip_cdata(result);
  result = xml_unescape(result);
  auto items = parse_didl(result);
  VRP_LOG("dlna: Browse %s id=%s → %zu entries", server.friendly_name.c_str(), oid.c_str(),
          items.size());
  if (!items.empty()) {
    const auto& s0 = items.front();
    VRP_LOG("dlna: sample title=\"%s\" id=%s container=%d playable=%d dtcp=%d mime=%s",
            s0.title.c_str(), s0.id.c_str(), static_cast<int>(s0.is_container),
            static_cast<int>(s0.playable), static_cast<int>(s0.dtcp_protected), s0.mime.c_str());
  } else if (result.size() > 80) {
    VRP_LOG("dlna: empty parse; DIDL head: %.120s", result.c_str());
  }
  return items;
}

}  // namespace vrp::dlna
