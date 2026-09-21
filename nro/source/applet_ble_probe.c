// Applet-side BLE connect probe. See the header for why this exists.

#include <dglab/nro/applet_ble_probe.h>
#include <dglab/nro/ble_poc_view.h>

#include <switch.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define PROBE_WAIT_STEP_MS 100u
#define PROBE_WAIT_LIMIT_MS 30000u
#define PROBE_MAX_SERVICES 8u

// Company id in the device's manufacturer specific data (the phone shows
// 0x000A). The btdrv-level scan finds the device with exactly this filter, so
// the general (manufacturer) scan is used here too: btm's smart device scan
// reported nothing at all (2026-09-22 hardware round).
#define PROBE_ADVERTISED_COMPANY_ID 0x000Au

// btm:u command 20, GetBleConnectionState. libnx has no wrapper; the request is
// built like the sysmodule's PoC builds it, with this applet's ARUID. This is
// what the connection state event is supposed to make readable.
static Result probeGetConnectionState(u64 aruid, BtdrvBleConnectionInfo* info, u8 count,
    u8* total_out)
{
    return serviceDispatchInOut(btmuGetServiceSession_IBtmUserCore(), 20, aruid, *total_out,
        .buffer_attrs = { SfBufferAttr_HipcPointer | SfBufferAttr_Out },
        .buffers = { { info, sizeof(BtdrvBleConnectionInfo) * count } },
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

    // Nintendo's flow is scan-then-connect: the connect is accepted (rc=0) but
    // nothing happens unless btm has seen the device itself. The general
    // (manufacturer data) scan is the one that actually reports this device.
    {
        BtdrvBleAdvertisePacketParameter param;
        BtdrvBleScanResult results[10];
        Event scan_event;
        bool found = false;
        u8 total = 0;

        memset(&param, 0, sizeof(param));
        param.company_id = PROBE_ADVERTISED_COMPANY_ID;

        rc = btdevAcquireBleScanEvent(&scan_event);
        probeLog("probe: AcquireBleScanEvent rc=0x%08X", (u32)rc);

        rc = btdevStartBleScanGeneral(param);
        probeLog("probe: StartBleScanGeneral(company=0x%04X) rc=0x%08X",
            (unsigned)PROBE_ADVERTISED_COMPANY_ID, (u32)rc);

        for (u32 poll = 0; poll < 12; poll++) {
            svcSleepThread(500000000ull); // 500ms
            total = 0;
            memset(results, 0, sizeof(results));
            rc = btdevGetBleScanResult(results, 10, &total);

            if (poll < 3 || (R_SUCCEEDED(rc) && total > 0))
                probeLog("probe: general poll %u rc=0x%08X total=%u", poll, (u32)rc, total);

            if (R_FAILED(rc) || total == 0)
                continue;

            for (u8 k = 0; k < total && k < 10; k++) {
                probeLog("probe:   dev[%u] %02X:%02X:%02X:%02X:%02X:%02X", k,
                    results[k].addr.address[0], results[k].addr.address[1],
                    results[k].addr.address[2], results[k].addr.address[3],
                    results[k].addr.address[4], results[k].addr.address[5]);

                if (memcmp(results[k].addr.address, addr->address, 6) == 0) {
                    found = true;
                } else if (!found && k == 0u) {
                    // Keep btm's own report as the connect target when it lists
                    // the device under an address we did not have.
                    memcpy(addr->address, results[k].addr.address, 6);
                }
            }

            if (found)
                break;
        }

        rc = btdevStopBleScanGeneral();
        probeLog("probe: StopBleScanGeneral rc=0x%08X (device reported=%u)", (u32)rc,
            found ? 1u : 0u);
        eventClose(&scan_event);
    }

    rc = btdevConnectToGattServer(*addr);
    probeLog("probe: btdevConnectToGattServer rc=0x%08X", (u32)rc);

    if (R_SUCCEEDED(rc)) {
        u32 events = 0;

        while (waited < PROBE_WAIT_LIMIT_MS) {
            Result wait_rc = eventWait(&event, 100000000ull); // 100ms

            waited += PROBE_WAIT_STEP_MS;

            if (R_SUCCEEDED(wait_rc)) {
                events++;
                probeLog("probe: connection state event #%u after %ums", events, waited);

                // The event says the state changed; btm:u command 20 is what
                // actually reports it.
                {
                    BtdrvBleConnectionInfo state[4];
                    u8 state_total = 0;
                    Result state_rc;

                    memset(state, 0, sizeof(state));
                    state_rc = probeGetConnectionState(appletGetAppletResourceUserId(), state, 4,
                        &state_total);
                    probeLog("probe:   GetConnectionState rc=0x%08X total=%u", (u32)state_rc,
                        state_total);

                    for (u8 k = 0; k < state_total && k < 4; k++)
                        probeLog("probe:     state[%u] handle=%u addr=%02X:%02X:%02X:%02X:%02X:%02X",
                            k, state[k].connection_handle, state[k].addr.address[0],
                            state[k].addr.address[1], state[k].addr.address[2],
                            state[k].addr.address[3], state[k].addr.address[4],
                            state[k].addr.address[5]);
                }
            }

            total = 0;
            memset(info, 0, sizeof(info));
            rc = btdevGetBleConnectionInfoList(info, 4, &total);

            if (R_FAILED(rc))
                continue;

            if (total != 0)
                probeLog("probe: connection list after %ums: total=%u handle=%u", waited, total,
                    info[0].connection_handle);

            if (total == 0) {
                // No connection yet: a state event without an entry means the
                // stack started something, so keep waiting either way.
                if (waited % 5000u < PROBE_WAIT_STEP_MS)
                    probeLog("probe: still waiting after %ums (state events=%u)", waited, events);
                continue;
            }

            handle = info[0].connection_handle;
            connected = true;
            break;
        }
    }

    if (!connected) {
        probeLog("probe: no connection after %ums", waited);
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
    probeLog("probe: disconnected");

    eventClose(&event);
    btdevExit();
}
