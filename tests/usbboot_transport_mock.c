/* SPDX-License-Identifier: GPL-3.0-or-later - native harness, no USB I/O. */
#define clock_gettime mock_clock_gettime
#define nanosleep mock_nanosleep
#define sleep mock_sleep
#define main usbboot_main
#include "../src/usbboot/usbboot.c"
#undef main
static uint64_t ticks;
static int scenario, scans, calls, controls;
static unsigned int previous_timeout = BULK_MS + 1;
static libusb_device *devices[] = {(void*)1, (void*)2, NULL};
int mock_clock_gettime(clockid_t id, struct timespec *ts) {
    ts->tv_sec = ticks / 1000; ts->tv_nsec = ticks % 1000 * 1000000; return 0;
}
int mock_nanosleep(const struct timespec *a, struct timespec *b) { ticks += 100; return 0; }
unsigned int mock_sleep(unsigned int seconds) {
    fprintf(stderr, "wait=%u\n", seconds); ticks += seconds * 1000; return 0;
}
int libusb_init(libusb_context **ctx) { return 0; }
void libusb_exit(libusb_context *ctx) {}
ssize_t libusb_get_device_list(libusb_context *ctx, libusb_device ***out) {
    *out = devices; ++scans;
    fprintf(stderr, "scan=%d\n", scans);
    if(scenario == 22 && controls) return LIBUSB_ERROR_IO;
    if(scenario == 10 || (scenario == 7 && scans < 4)) return 0;
    return scenario == 9 ? 2 : 1;
}
void libusb_free_device_list(libusb_device **d, int unref) {}
int libusb_get_device_descriptor(libusb_device *d, struct libusb_device_descriptor *desc) {
    if(scenario == 14 || (scenario == 23 && controls)) return -1;
    desc->idVendor = 0xa108; desc->idProduct = 0xeaef; return 0;
}
uint8_t libusb_get_bus_number(libusb_device *d) { return scenario == 16 ? 2 : 1; }
uint8_t libusb_get_device_address(libusb_device *d) { return 9; }
int libusb_get_port_numbers(libusb_device *d, uint8_t *ports, int len) {
    ports[0] = scenario == 8 ? 2 : 1;
    return scenario == 15 || (scenario == 24 && controls) ? 0 : 1;
}
int libusb_open(libusb_device *d, libusb_device_handle **h) { *h = (void*)1; return 0; }
void libusb_close(libusb_device_handle *h) {}
int libusb_claim_interface(libusb_device_handle *h, int i) { return 0; }
int libusb_release_interface(libusb_device_handle *h, int i) { return 0; }
int libusb_control_transfer(libusb_device_handle *h, uint8_t type, uint8_t req,
    uint16_t value, uint16_t index, unsigned char *data, uint16_t len, unsigned int timeout) {
    if(req == LIBUSB_REQUEST_GET_CONFIGURATION) {
        fprintf(stderr, "get-configuration\n");
        *data = 1; return scenario == 7 ? LIBUSB_ERROR_NO_DEVICE : 1;
    }
    ++controls;
    fprintf(stderr, "control=%u call=%d\n", req, controls);
    if(scenario == 11 || scenario == 21) return LIBUSB_ERROR_TIMEOUT;
    if(data) memset(data, 'X', len);
    return len;
}
int libusb_bulk_transfer(libusb_device_handle *h, unsigned char endpoint,
    unsigned char *data, int len, int *actual, unsigned int timeout) {
    ++calls;
    if(timeout == BULK_MS) previous_timeout = BULK_MS + 1;
    if(len > BULK_BYTES || !timeout || timeout > previous_timeout) abort();
    previous_timeout = timeout;
    *actual = len > 3 ? 3 : len;
    ticks += scenario == 6 ? 300000 : 1;
    fprintf(stderr, "bulk-call=%d endpoint=%u\n", calls, endpoint);
    if(scenario == 19 && calls == 2) return LIBUSB_ERROR_TIMEOUT;
    if(scenario == 2) return LIBUSB_ERROR_TIMEOUT;
    if(scenario == 3) *actual = 0;
    if(scenario == 4) *actual = -1;
    if(scenario == 5) *actual = len + 1;
    if(scenario == 17) return LIBUSB_ERROR_NO_DEVICE;
    if(scenario == 18) return 1;
    memset(data, 'X', *actual); return 0;
}
int main(int argc, char **argv) {
    scenario = atoi(argv[1]);
    if(scenario == 13 || scenario == 19 || scenario >= 22) return usbboot_main(argc - 1, argv + 1);
    apply_cpu_profile("x2000");
    g_usb_dev = (void*)1;
    if(scenario >= 7 && scenario <= 10 || scenario >= 14 && scenario <= 16) {
        saved_location = true; saved_bus = 1; saved_depth = 1; saved_ports[0] = 1;
        stage1_pending = true; observe_stage1(); open_usb();
        fprintf(stderr, "reconnect-ms=%llu\n", (unsigned long long)ticks);
    } else if(scenario == 11 || scenario == 12 || scenario == 21) {
        g_cmd_retries = 5;
        observe_spl = true;
        if(scenario == 11) jz_program_start2(0xa0c00030);
        else jz_program_start1(0xb2401800);
    } else {
        unsigned char data[10] = {0};
        if(atoi(argv[2])) bulk_transfer_in(data, sizeof(data));
        else bulk_transfer_out(data, sizeof(data));
    }
    return 0;
}
