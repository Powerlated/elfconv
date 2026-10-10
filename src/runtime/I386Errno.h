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

#if defined(__EMSCRIPTEN__)
#undef ECV_I386_ERRNOS
#endif
