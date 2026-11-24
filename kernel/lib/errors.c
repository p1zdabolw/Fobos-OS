/*
 * Fobos OS — строковые представления кодов ошибок (общая функция).
 */
#include <fobos/errors.h>

const char *k_strerror(err_t code)
{
    switch (code) {
    case K_OK:            return "ok";
    case K_EINVAL:        return "invalid argument";
    case K_ENOMEM:        return "out of memory";
    case K_ENODEV:        return "no device";
    case K_EIO:           return "i/o error";
    case K_EUNSUPPORTED:  return "unsupported";
    case K_ESTATE:        return "invalid state";
    case K_ERANGE:        return "out of range";
    case K_EAGAIN:        return "try again";
    case K_ENOENT:        return "not found";
    case K_EEXIST:        return "already exists";
    case K_EPERM:         return "not permitted";
    case K_ENOSYS:        return "not implemented";
    default:              return "unknown error";
    }
}
