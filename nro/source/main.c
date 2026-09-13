#include <stdio.h>

#include <switch.h>

#include <dglab/ipc.h>

int main(int argc, char* argv[])
{
    (void)argc;
    (void)argv;

    consoleInit(NULL);

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    Service dglab;
    Result rc = smGetService(&dglab, DGLAB_IPC_SERVICE_NAME);

    if (R_FAILED(rc)) {
        printf("DGLAB sysmodule not found (0x%08X)\n", rc);
        printf("Install the sysmodule and reboot the console.\n");
    } else {
        DglabIpcVersion version = {0};
        rc = serviceDispatchOut(&dglab, DGLAB_IPC_CMD_GET_VERSION, version);
        if (R_SUCCEEDED(rc)) {
            printf("DGLAB sysmodule connected\n");
            printf("IPC version: %u.%u.%u\n", version.major, version.minor, version.patch);
        } else {
            printf("GetVersion failed (0x%08X)\n", rc);
        }
        serviceClose(&dglab);
    }

    printf("\nPress + to exit.\n");
    consoleUpdate(NULL);

    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus)
            break;
        consoleUpdate(NULL);
    }

    consoleExit(NULL);
    return 0;
}
