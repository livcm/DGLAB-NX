// Applet-side BLE connect probe. See the header for why this exists.

#include <dglab/nro/applet_ble_probe.h>
#include <dglab/nro/ble_poc_view.h>

#include <switch.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define PROBE_WAIT_STEP_MS 100u
#define PROBE_WAIT_LIMIT_MS 12000u
#define PROBE_MAX_SERVICES 8u

// The service UUID the device advertises (docs/dglab-protocol.md). Repeated
// here because the NRO does not include the sysmodule's protocol headers.
#define PROBE_ADVERTISED_UUID16 0x180Cu

// btm:u command 10, GetBleScanResultsForSmartDevice. libnx has no wrapper for
// it, so the request is built here the same way the sysmodule's PoC builds it
// (docs/ble-poc.md) - in this process the ARUID is a real applet's.
static Result probeGetSmartScanResults(u64 aruid, BtdrvBleScanResult* results, u8 count,
    u8* total_out)
{
    return serviceDispatchInOut(btmuGetServiceSession_IBtmUserCore(), 10, aruid, *total_out,
        .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_Out },
        .buffers = { { results, sizeof(BtdrvBleScanResult) * count } },
        .in_send_pid = true,
    );
}

// Print to the console and mirror the line into the view's log file, so a run
// can be reported back without photographing the screen.
static void probeLog(const char* fmt, ...)
{
    char line[192];
    va_list args;

    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);

    printf("%s\n", line);
    dglabBlePocViewLogLine(line);
    consoleUpdate(NULL);
}

static void probeUuid(char* out, size_t out_size, const BtdrvGattAttributeUuid* uuid)
{
    if (uuid->size == 2) {
        snprintf(out, out_size, "0x%04X", (unsigned)(uuid->uuid[0] | (uuid->uuid[1] << 8)));
    } else if (uuid->size == 4) {
        snprintf(out, out_size, "0x%08X",
            (unsigned)(uuid->uuid[0] | (uuid->uuid[1] << 8) | (uuid->uuid[2] << 16) |
                (uuid->uuid[3] << 24)));
    } else {
        size_t used = 0;

        for (size_t i = 0; i < sizeof(uuid->uuid) && used + 3u < out_size; i++) {
            int written = snprintf(out + used, out_size - used, "%02X", uuid->uuid[i]);

            if (written <= 0)
                break;
            used += (size_t)written;
        }
    }
}

void dglabAppletBleProbeRun(BtdrvAddress* addr, const char* address_path)
{
    BtdrvBleConnectionInfo info[4];
    BtdevGattService services[PROBE_MAX_SERVICES];
    char uuid_text[40];
    Event event;
    Result rc;
    u32 handle = 0xFFFFFFFFu;
    u32 waited = 0u;
    u8 total = 0;
    bool connected = false;

    probeLog("=== applet-side BLE probe (btm:u with this applet's ARUID) ===");
    probeLog("target: %02X:%02X:%02X:%02X:%02X:%02X (from %s)", addr->address[0],
        addr->address[1], addr->address[2], addr->address[3], addr->address[4],
        addr->address[5], address_path);

    rc = btdevInitialize();
    probeLog("probe: btdevInitialize rc=0x%08X", (u32)rc);

    if (R_FAILED(rc))
        return;

    rc = btdevAcquireBleConnectionStateChangedEvent(&event);
    probeLog("probe: AcquireBleConnectionStateChangedEvent rc=0x%08X", (u32)rc);

    // Nintendo's flow is scan-then-connect: btm:u's smart device scan tells btm
    // which device the caller is interested in, and the connect then has an
    // address it has actually seen. Without this the connect is accepted
    // (rc=0) but nothing happens (2026-09-22 hardware round).
    {
        BtdrvGattAttributeUuid uuid = { 0 };

        uuid.size = 2;
        uuid.uuid[0] = (u8)(PROBE_ADVERTISED_UUID16 & 0xFFu);
        uuid.uuid[1] = (u8)(PROBE_ADVERTISED_UUID16 >> 8);

        rc = btdevStartBleScanSmartDevice(&uuid);
        probeLog("probe: StartBleScanSmartDevice(0x%04X) rc=0x%08X",
            (unsigned)PROBE_ADVERTISED_UUID16, (u32)rc);

        // Read btm's smart-device scan results: the connect only counts for an
        // address btm itself has reported. The configured address is kept as a
        // fallback.
        {
            BtdrvBleScanResult results[4];
            u64 aruid = appletGetAppletResourceUserId();
            u8 total = 0;

            for (u32 poll = 0; poll < 10; poll++) {
                svcSleepThread(500000000ull); // 500ms
                total = 0;
                memset(results, 0, sizeof(results));
                rc = probeGetSmartScanResults(aruid, results, 4, &total);

                if (poll < 3 || (R_SUCCEEDED(rc) && total > 0))
                    probeLog("probe: smart scan poll %u rc=0x%08X total=%u", poll, (u32)rc,
                        total);

                if (R_FAILED(rc) || total == 0)
                    continue;

                for (u8 k = 0; k < total && k < 4; k++)
                    probeLog("probe:   smart[%u] %02X:%02X:%02X:%02X:%02X:%02X",
                        k, results[k].addr.address[0], results[k].addr.address[1],
                        results[k].addr.address[2], results[k].addr.address[3],
                        results[k].addr.address[4], results[k].addr.address[5]);

                {
                    BtdrvAddress first = results[0].addr;

                    memcpy(addr, first.address, sizeof(addr->address));
                    probeLog("probe: connecting to the address btm reported");
                }
                break;
            }
        }
    }

    rc = btdevConnectToGattServer(*addr);
    probeLog("probe: btdevConnectToGattServer rc=0x%08X", (u32)rc);

    if (R_SUCCEEDED(rc)) {
        while (waited < PROBE_WAIT_LIMIT_MS) {
            eventWait(&event, 100000000ull); // 100ms
            waited += PROBE_WAIT_STEP_MS;

            total = 0;
            memset(info, 0, sizeof(info));
            rc = btdevGetBleConnectionInfoList(info, 4, &total);

            if (R_FAILED(rc) || total == 0)
                continue;

            handle = info[0].connection_handle;
            connected = true;
            break;
        }
    }

    if (!connected) {
        probeLog("probe: no connection after %ums\n", waited);
        probeLog("probe: StopBleScanSmartDevice rc=0x%08X",
            (u32)btdevStopBleScanSmartDevice());
        eventClose(&event);
        btdevExit();
        return;
    }

    probeLog("probe: connected after %ums, handle=%u addr=%02X:%02X:%02X:%02X:%02X:%02X",
        waited, handle, info[0].addr.address[0], info[0].addr.address[1],
        info[0].addr.address[2], info[0].addr.address[3], info[0].addr.address[4],
        info[0].addr.address[5]);

    total = 0;
    memset(services, 0, sizeof(services));
    rc = btdevGetGattServices(handle, services, PROBE_MAX_SERVICES, &total);
    probeLog("probe: GetGattServices rc=0x%08X total=%u", (u32)rc, total);

    for (u32 i = 0; i < total && i < PROBE_MAX_SERVICES; i++) {
        probeUuid(uuid_text, sizeof(uuid_text), &services[i].attr.uuid);
        probeLog("probe:   service[%u] uuid=%s handle=%u end=%u primary=%u", i, uuid_text,
            services[i].attr.handle, services[i].end_group_handle,
            services[i].primary_service ? 1u : 0u);
    }

    btdevDisconnectFromGattServer(handle);
    probeLog("probe: disconnected, StopBleScanSmartDevice rc=0x%08X",
        (u32)btdevStopBleScanSmartDevice());

    eventClose(&event);
    btdevExit();
}
