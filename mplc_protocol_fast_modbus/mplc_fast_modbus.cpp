#include <addincmn.h>
#include <drivers/drv_user.h>
#include <mplc/api.h>
#include <mplc/api/macros.h>
#include "fast_modbus_protocol.h"
MPLC_DECLARE_PROPERTIES(FastModbusProtocol) {
    MPLC_In(PortName),MPLC_In(BaudRate),MPLC_In(Parity),MPLC_In(StopBits),MPLC_In(DataBits),
    MPLC_In(ResponseTimeoutMs),MPLC_In(InterFrameDelayMs),MPLC_In(EventPollIntervalMs),
    MPLC_In(FallbackPollPeriodMs),MPLC_In(HealthPollPeriodMs),MPLC_In(DeviceTimeoutMs),
    MPLC_In(MaxTransactionsPerCycle),MPLC_In(MaxCycleMs),MPLC_In(EnableFastModbus),MPLC_In(AutoScan),MPLC_In(AllowWrites),
    MPLC_Out(Requests),MPLC_Out(Responses),MPLC_Out(Timeouts),MPLC_Out(BadFrames),
    MPLC_Out(EventPackets),MPLC_Out(EventValues),MPLC_Out(DuplicatePackets),MPLC_Out(ReadRequests),
    MPLC_Out(WriteRequests),MPLC_Out(WriteErrors),MPLC_Out(RejectedSubscriptions),MPLC_Out(ScannedDevices),
};
MPLC_PROTOCOL_TYPE(FastModbus,FastModbusProtocol);
EXTERN_C MPLC_DRIVER_API int InitAddin(ProcessRequestCallback,int,int*) {
    PRINTLN("FastModbus WB bench candidate 20261122 loaded");return 0;
}
