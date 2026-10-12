#pragma once

#include <cerrno>

// Emscripten uses WASI errno numbers; lifted i386 code uses Linux numbers.
#if defined(__EMSCRIPTEN__)
#define ECV_I386_ERRNOS(X) \
  X(EPERM, 1) \
  X(ENOENT, 2) \
  X(ESRCH, 3) \
  X(EINTR, 4) \
  X(EIO, 5) \
  X(ENXIO, 6) \
  X(E2BIG, 7) \
  X(ENOEXEC, 8) \
  X(EBADF, 9) \
  X(ECHILD, 10) \
  X(EAGAIN, 11) \
  X(ENOMEM, 12) \
  X(EACCES, 13) \
  X(EFAULT, 14) \
  X(ENOTBLK, 15) \
  X(EBUSY, 16) \
  X(EEXIST, 17) \
  X(EXDEV, 18) \
  X(ENODEV, 19) \
  X(ENOTDIR, 20) \
  X(EISDIR, 21) \
  X(EINVAL, 22) \
  X(ENFILE, 23) \
  X(EMFILE, 24) \
  X(ENOTTY, 25) \
  X(ETXTBSY, 26) \
  X(EFBIG, 27) \
  X(ENOSPC, 28) \
  X(ESPIPE, 29) \
  X(EROFS, 30) \
  X(EMLINK, 31) \
  X(EPIPE, 32) \
  X(EDOM, 33) \
  X(ERANGE, 34) \
  X(EDEADLK, 35) \
  X(ENAMETOOLONG, 36) \
  X(ENOLCK, 37) \
  X(ENOSYS, 38) \
  X(ENOTEMPTY, 39) \
  X(ELOOP, 40) \
  X(ENOMSG, 42) \
  X(EIDRM, 43) \
  X(ECHRNG, 44) \
  X(EL2NSYNC, 45) \
  X(EL3HLT, 46) \
  X(EL3RST, 47) \
  X(ELNRNG, 48) \
  X(EUNATCH, 49) \
  X(ENOCSI, 50) \
  X(EL2HLT, 51) \
  X(EBADE, 52) \
  X(EBADR, 53) \
  X(EXFULL, 54) \
  X(ENOANO, 55) \
  X(EBADRQC, 56) \
  X(EBADSLT, 57) \
  X(EBFONT, 59) \
  X(ENOSTR, 60) \
  X(ENODATA, 61) \
  X(ETIME, 62) \
  X(ENOSR, 63) \
  X(ENONET, 64) \
  X(ENOPKG, 65) \
  X(EREMOTE, 66) \
  X(ENOLINK, 67) \
  X(EADV, 68) \
  X(ESRMNT, 69) \
  X(ECOMM, 70) \
  X(EPROTO, 71) \
  X(EMULTIHOP, 72) \
  X(EDOTDOT, 73) \
  X(EBADMSG, 74) \
  X(EOVERFLOW, 75) \
  X(ENOTUNIQ, 76) \
  X(EBADFD, 77) \
  X(EREMCHG, 78) \
  X(ELIBACC, 79) \
  X(ELIBBAD, 80) \
  X(ELIBSCN, 81) \
  X(ELIBMAX, 82) \
  X(ELIBEXEC, 83) \
  X(EILSEQ, 84) \
  X(ERESTART, 85) \
  X(ESTRPIPE, 86) \
  X(EUSERS, 87) \
  X(ENOTSOCK, 88) \
  X(EDESTADDRREQ, 89) \
  X(EMSGSIZE, 90) \
  X(EPROTOTYPE, 91) \
  X(ENOPROTOOPT, 92) \
  X(EPROTONOSUPPORT, 93) \
  X(ESOCKTNOSUPPORT, 94) \
  X(EOPNOTSUPP, 95) \
  X(EPFNOSUPPORT, 96) \
  X(EAFNOSUPPORT, 97) \
  X(EADDRINUSE, 98) \
  X(EADDRNOTAVAIL, 99) \
  X(ENETDOWN, 100) \
  X(ENETUNREACH, 101) \
  X(ENETRESET, 102) \
  X(ECONNABORTED, 103) \
  X(ECONNRESET, 104) \
  X(ENOBUFS, 105) \
  X(EISCONN, 106) \
  X(ENOTCONN, 107) \
  X(ESHUTDOWN, 108) \
  X(ETOOMANYREFS, 109) \
  X(ETIMEDOUT, 110) \
  X(ECONNREFUSED, 111) \
  X(EHOSTDOWN, 112) \
  X(EHOSTUNREACH, 113) \
  X(EALREADY, 114) \
  X(EINPROGRESS, 115) \
  X(ESTALE, 116) \
  X(EUCLEAN, 117) \
  X(ENOTNAM, 118) \
  X(ENAVAIL, 119) \
  X(EISNAM, 120) \
  X(EREMOTEIO, 121) \
  X(EDQUOT, 122) \
  X(ENOMEDIUM, 123) \
  X(EMEDIUMTYPE, 124) \
  X(ECANCELED, 125) \
  X(ENOKEY, 126) \
  X(EKEYEXPIRED, 127) \
  X(EKEYREVOKED, 128) \
  X(EKEYREJECTED, 129) \
  X(EOWNERDEAD, 130) \
  X(ENOTRECOVERABLE, 131) \
  X(ERFKILL, 132) \
  X(EHWPOISON, 133)
#endif

inline int host_errno_to_i386(int value) {
#if defined(__EMSCRIPTEN__)
  switch (value) {
#define ECV_ERRNO_CASE(name, number) case name: return number;
    ECV_I386_ERRNOS(ECV_ERRNO_CASE)
#undef ECV_ERRNO_CASE
    default: return value;
  }
#else
  return value;
#endif
}

inline int i386_errno_to_host(int value) {
#if defined(__EMSCRIPTEN__)
  switch (value) {
#define ECV_ERRNO_CASE(name, number) case number: return name;
    ECV_I386_ERRNOS(ECV_ERRNO_CASE)
#undef ECV_ERRNO_CASE
    default: return value;
  }
#else
  return value;
#endif
}

inline const char *i386_strerrordesc(int value) {
#if defined(__EMSCRIPTEN__)
  switch (value) {
    case 0: return "Success";
    case 5: return "Input/output error";
    case 10: return "No child processes";
    case 35: return "Resource deadlock avoided";
    case 12: return "Cannot allocate memory";
    case 16: return "Device or resource busy";
    case 18: return "Invalid cross-device link";
    case 24: return "Too many open files";
    case 25: return "Inappropriate ioctl for device";
    case 29: return "Illegal seek";
    case 33: return "Numerical argument out of domain";
    case 34: return "Numerical result out of range";
    case 115: return "Operation now in progress";
    case 88: return "Socket operation on non-socket";
    case 90: return "Message too long";
    case 95: return "Operation not supported";
    case 98: return "Address already in use";
    case 99: return "Cannot assign requested address";
    case 101: return "Network is unreachable";
    case 102: return "Network dropped connection on reset";
    case 103: return "Software caused connection abort";
    case 106: return "Transport endpoint is already connected";
    case 107: return "Transport endpoint is not connected";
    case 108: return "Cannot send after transport endpoint shutdown";
    case 109: return "Too many references: cannot splice";
    case 110: return "Connection timed out";
    case 40: return "Too many levels of symbolic links";
    case 36: return "File name too long";
    case 113: return "No route to host";
    case 87: return "Too many users";
    case 122: return "Disk quota exceeded";
    case 66: return "Object is remote";
    case 84: return "Invalid or incomplete multibyte or wide character";
    case 62: return "Timer expired";
    case 85: return "Interrupted system call should be restarted";
    case 44: return "Channel number out of range";
    case 45: return "Level 2 not synchronized";
    case 46: return "Level 3 halted";
    case 47: return "Level 3 reset";
    case 48: return "Link number out of range";
    case 49: return "Protocol driver not attached";
    case 50: return "No CSI structure available";
    case 51: return "Level 2 halted";
    case 52: return "Invalid exchange";
    case 53: return "Invalid request descriptor";
    case 54: return "Exchange full";
    case 55: return "No anode";
    case 56: return "Invalid request code";
    case 57: return "Invalid slot";
    case 59: return "Bad font file format";
    case 64: return "Machine is not on the network";
    case 65: return "Package not installed";
    case 68: return "Advertise error";
    case 69: return "Srmount error";
    case 70: return "Communication error on send";
    case 73: return "RFS specific error";
    case 76: return "Name not unique on network";
    case 78: return "Remote address changed";
    case 79: return "Can not access a needed shared library";
    case 80: return "Accessing a corrupted shared library";
    case 81: return ".lib section in a.out corrupted";
    case 82: return "Attempting to link in too many shared libraries";
    case 83: return "Cannot exec a shared library directly";
    case 86: return "Streams pipe error";
    case 117: return "Structure needs cleaning";
    case 118: return "Not a XENIX named type file";
    case 119: return "No XENIX semaphores available";
    case 120: return "Is a named type file";
    case 132: return "Operation not possible due to RF-kill";
    case 133: return "Memory page has hardware error";
    default: return nullptr;
  }
#else
  (void)value;
  return nullptr;
#endif
}

inline const char *i386_strerrorname(int value) {
#if defined(__EMSCRIPTEN__)
  if (value == 0) return "0";
  switch (value) {
#define ECV_ERRNO_NAME(name, number) case number: return #name;
    ECV_I386_ERRNOS(ECV_ERRNO_NAME)
#undef ECV_ERRNO_NAME
    default: return nullptr;
  }
#else
  (void)value;
  return nullptr;
#endif
}

#if defined(__EMSCRIPTEN__)
#undef ECV_I386_ERRNOS
#endif
