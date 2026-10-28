#pragma once

/** Line protocol between monaSphere and LD_PRELOAD libmonasphere_padhook.so */
namespace vrp {
namespace pad_hook {

/** Env: absolute path of the Unix socket (server listens, guest connects). */
inline constexpr const char* kEnvSock = "MONASPHERE_PAD_SOCK";
/** Env: absolute path of libmonasphere_padhook.so (optional; for debugging). */
inline constexpr const char* kEnvLib = "MONASPHERE_PAD_HOOK";

/** Guest → host */
inline constexpr const char* kMsgHello = "HELLO";
inline constexpr const char* kMsgMenu = "MENU";
inline constexpr const char* kMsgRecenter = "RECENTER";

/** Host → guest */
inline constexpr const char* kMsgBlockOn = "BLOCK 1";
inline constexpr const char* kMsgBlockOff = "BLOCK 0";

}  // namespace pad_hook
}  // namespace vrp
