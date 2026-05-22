// DLL/SO entry point and registration macros for the Fast Modbus protocol.
// This file must be compiled INTO the shared library (not excluded by Makefile's EXCLUDE pattern).

#include <addincmn.h>
#include <drivers/drv_user.h>
#include <mplc/api.h>
#include <mplc/api/macros.h>

#include "fast_modbus_protocol.h"

// ---- Protocol registration ----
// Name "FastModbus" is how the protocol appears in the MasterSCADA 4D project tree.
MPLC_PROTOCOL_TYPE(FastModbus, FastModbusProtocol);

// ---- Configurable properties of FastModbusProtocol ----
// These fields appear in the MS4 configuration dialog for the protocol node.
MPLC_DECLARE_PROPERTIES(FastModbusProtocol) {
    MPLC_In(PortName),
    MPLC_In(BaudRate),
    MPLC_In(Parity),
    MPLC_In(StopBits),
    MPLC_In(DataBits),
    MPLC_In(ResponseTimeoutMs),
    MPLC_In(InterFrameDelayMs),
    MPLC_In(EventPollIntervalMs),
    MPLC_In(FallbackPollPeriodMs),
    MPLC_In(EnableFastModbus),
    MPLC_In(AutoScan),
};

// ---- DLL/SO addin entry point ----
static int AddinInit()
{
    PRINTLN("FastModbus protocol loaded");
    return 0;
}

EXTERN_C MPLC_DRIVER_API int InitAddin(ProcessRequestCallback /*func*/,
                                        int /*nInFlags*/, int* /*pnOutFlags*/)
{
    return AddinInit();
}
