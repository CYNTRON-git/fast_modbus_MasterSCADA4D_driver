#include "mplc_cyntron.h"
#include "modbus_rtu_protocol.h"
#include "drivers/drv_user.h"
#include "addincmn.h"

EXTERN_C MPLC_DRIVER_API int InitAddin(ProcessRequestCallback func, int nInFlags, int* pnOutFlags) {
    return S_OK;
}

EXTERN_C MPLC_DRIVER_API void DisposeAddin() {}
