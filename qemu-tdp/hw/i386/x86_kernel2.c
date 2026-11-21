#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "hw/i386/x86.h"
#include "hw/nvram/fw_cfg.h"
#include "hw/loader.h"

void x86_load_linux2(X86MachineState *x86ms, FWCfgState *fw_cfg);

void x86_load_linux2(X86MachineState *x86ms, FWCfgState *fw_cfg)
{
    MachineState *machine = MACHINE(x86ms);
    const char *kernel2_filename = machine->kernel2_filename;
    const char *initrd2_filename = machine->initrd2_filename;
    const char *kernel2_cmdline = machine->kernel2_cmdline;
    
    if (!kernel2_filename) {
        return;
    }
    
    FILE *f = fopen(kernel2_filename, "rb");
    if (!f) {
        error_report("qemu: could not open kernel2 file '%s'", kernel2_filename);
        exit(1);
    }
    fseek(f, 0, SEEK_END);

    size_t kernel2_file_size = ftell(f);

    size_t kernel2_size = kernel2_file_size;

    fseek(f, 0, SEEK_SET);



    uint8_t *bzimage2 = g_malloc(kernel2_file_size);

    if (fread(bzimage2, 1, kernel2_file_size, f) != kernel2_file_size) {

        error_report("qemu: could not read kernel2 file '%s'", kernel2_filename);

        exit(1);

    }



    uint8_t header[8192];

    memcpy(header, bzimage2, MIN(sizeof(header), kernel2_file_size));



    int setup_size = header[0x1f1];
    if (setup_size == 0) {
        setup_size = 4;
    }
    setup_size = (setup_size + 1) * 512;
    if (setup_size > kernel2_size) {
        error_report("qemu: invalid kernel2 header");
        exit(1);
    }
    kernel2_size -= setup_size;
    
    uint8_t *setup2 = g_malloc(setup_size);

    uint8_t *kernel2 = g_malloc(kernel2_size);

    memcpy(setup2, bzimage2, setup_size);

    memcpy(kernel2, bzimage2 + setup_size, kernel2_size);

    fclose(f);



    fw_cfg_add_file(fw_cfg, "opt/bzimage2", bzimage2, kernel2_file_size);

    fw_cfg_add_file(fw_cfg, "opt/setup2", setup2, setup_size);

    fw_cfg_add_file(fw_cfg, "opt/kernel2", kernel2, kernel2_size);



    if (initrd2_filename) {
        f = fopen(initrd2_filename, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            size_t initrd2_size = ftell(f);
            fseek(f, 0, SEEK_SET);
            uint8_t *initrd2 = g_malloc(initrd2_size);
            if (fread(initrd2, 1, initrd2_size, f) == initrd2_size) {
                fw_cfg_add_file(fw_cfg, "opt/initrd2", initrd2, initrd2_size);
            }
            fclose(f);
        }
    }
    
    if (kernel2_cmdline) {
        size_t cmdline2_size = strlen(kernel2_cmdline) + 1;
        uint8_t *cmdline2 = g_malloc(cmdline2_size);
        memcpy(cmdline2, kernel2_cmdline, cmdline2_size);
        fw_cfg_add_file(fw_cfg, "opt/cmdline2", cmdline2, cmdline2_size);
    }
}
