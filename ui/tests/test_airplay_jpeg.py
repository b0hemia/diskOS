# SPDX-License-Identifier: GPL-3.0-or-later
"""AirPlay artwork encoding and outgoing request; requires GCC and Pillow."""
import io
import os
import re
import subprocess
import tempfile
import unittest
from pathlib import Path

from PIL import Image

APP = Path(__file__).resolve().parents[1]


def function(text, name):
    start = re.search(r'^(?:static )?int ' + name + r'\([^;{}]*\)\s*\{',
                      text, re.M).start()
    pos = text.index('{', start)
    tokens = re.finditer(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|'
                         r"'(?:\\.|[^'\\])*'|[{}]", text[pos:], re.S)
    depth = 0
    for token in tokens:
        if token.group() == '{':
            depth += 1
        elif token.group() == '}':
            depth -= 1
            if depth == 0:
                return text[start:pos + token.end()]
    raise AssertionError('unclosed function: ' + name)


class AirPlayArtTests(unittest.TestCase):
    def test_jpeg_and_png_fallback_requests(self):
        session = (APP / 'airplay_session.c').read_text()
        self.assertIn('cast_art_airplay_fd(t->art_fd,',
                      (APP / 'airplay_worker.c').read_text())
        probe = r"""
#include <assert.h>
#include <stdlib.h>
#include "cast_art.h"
static int fail_alloc;
static void *art_malloc(size_t n) {
    if (fail_alloc && n == CA_JPEG_MAX) { fail_alloc = 0; return NULL; }
    return malloc(n);
}
#define malloc art_malloc
#include "cast_art.c"
#undef malloc
void lv_memset(void *d, uint8_t v, size_t n) { if(n) memset(d, v, n); }
int sd_io_begin(void) { return 1; }
void sd_io_end(void) {}
int sd_io_allowed(void) { return 1; }
uint64_t sd_io_generation(void) { return 0; }
unsigned artcache_gen(void) { return 0; }
int artcache_cover_candidate(const char *p, char *o, int n) {
    (void)p; (void)o; (void)n; return -1;
}
typedef struct { int status; } reply;
typedef struct { char uri[8]; } rtsp;
typedef struct { uint16_t seq; } rtp_state;
typedef struct { rtsp rtsp; reply reply; int error; rtp_state rtp; } aps_session;
enum { APS_ERR_BUSY, APS_ERR_CONNECT };
/* Capture the real parameter() request at the RTSP transport boundary. */
static int aps_rtsp_request(rtsp *r, const char *method, const char *uri,
                           const char *h, const void *p, size_t n, reply *out, unsigned ms) {
    (void)r;
    assert(!strcmp(method, "SET_PARAMETER") && !strcmp(uri, "*") && ms == 2000);
    assert(strstr(h, "RTP-Info: seq=0;rtptime=1234\r\n"));
    assert(fwrite(h, 1, strlen(h), stdout) == strlen(h));
    assert(fwrite("\r\n", 1, 2, stdout) == 2);
    assert(fwrite(p, 1, n, stdout) == n);
    out->status = 200; return 0;
}
""" + '\n'.join(function(session, n) for n in
               ('okay', 'parameter', 'aps_session_art')) + r"""
int main(int argc, char **argv) {
    assert(argc == 3);
    unsigned char *guard = malloc(CA_JPEG_MAX + 2), byte = 7;
    assert(guard); memset(guard, 0xa5, CA_JPEG_MAX + 2);
    ca_jpeg sink = {guard + 1, CA_JPEG_MAX - 1, 0};
    jpeg_write(&sink, &byte, 1);
    assert(!sink.failed && sink.used == CA_JPEG_MAX && guard[CA_JPEG_MAX] == 7);
    jpeg_write(&sink, &byte, 1);
    assert(sink.failed && guard[0] == 0xa5 && guard[CA_JPEG_MAX + 1] == 0xa5);
    sink.used = 0; jpeg_write(&sink, &byte, 1);
    assert(guard[1] == 0xa5); /* overflow is sticky */
    sink.failed = 0; jpeg_write(&sink, &byte, -1);
    assert(sink.failed && sink.used == 0); free(guard);
    unsigned char *data = NULL; size_t len = 0;
    fail_alloc = !strcmp(argv[2], "alloc");
    if (!strcmp(argv[2], "empty")) assert(!cast_art_empty_png(&data, &len));
    else {
        int fd = open(argv[1], O_RDONLY);
        assert(fd >= 0 && lseek(fd, 7, SEEK_SET) == 7);
        if (!strcmp(argv[2], "cast")) assert(!cast_art_png_fd(fd, &data, &len));
        else assert(!cast_art_airplay_fd(fd, &data, &len));
        assert(lseek(fd, 0, SEEK_CUR) == 7); close(fd);
    }
    assert(data && len && len <= 256 * 1024 && !fail_alloc);
    aps_session s = {0}; strcpy(s.rtsp.uri, "*");
    assert(aps_session_art(&s, NULL, 0, 1234) == -1);
    assert(aps_session_art(&s, "bad", 3, 1234) == -1);
    assert(aps_session_art(&s, data, 256u * 1024u + 1, 1234) == -1);
    assert(!aps_session_art(&s, data, len, 1234));
    free(data); return 0;
}
"""
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            image = Image.frombytes('RGB', (148, 148), bytes(
                c for y in range(148) for x in range(148)
                for c in (x * 255 // 147, y * 255 // 147, 91)))
            bmp = d / 'cover.bmp'
            image.save(bmp)
            source = d / 'jpeg.c'
            source.write_text(probe)
            for cap in (256 * 1024, 64):
                exe = d / ('jpeg-' + str(cap))
                build = subprocess.run([
                    'gcc', '-std=gnu11', '-Os', '-Wall', '-Wextra', '-Werror',
                    '-pthread', '-DLV_CONF_INCLUDE_SIMPLE', '-DCA_JPEG_MAX=' + str(cap),
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    '-fno-omit-frame-pointer', '-ffp-contract=off',
                    '-I' + str(APP), '-I' + str(APP.parent), str(source), '-lm', '-o', str(exe)
                ], capture_output=True, text=True)
                self.assertEqual(build.returncode, 0, build.stderr)
                for mode in ('normal', 'alloc', 'empty', 'cast'):
                    run = subprocess.run([str(exe), str(bmp), mode], capture_output=True,
                                         timeout=20, env=dict(os.environ, ASAN_OPTIONS='detect_leaks=1'))
                    self.assertEqual(run.returncode, 0, run.stderr.decode())
                    headers, body = run.stdout.split(b'\r\n\r\n', 1)
                    jpeg = cap > 64 and mode == 'normal'
                    self.assertIn(b'Content-Type: image/' + (b'jpeg' if jpeg else b'png'), headers)
                    self.assertTrue(0 < len(body) <= 256 * 1024)
                    if jpeg:
                        self.assertEqual(body[:2], b'\xff\xd8')
                        self.assertEqual(body[-2:], b'\xff\xd9')
                    else:
                        self.assertTrue(body.startswith(b'\x89PNG\r\n\x1a\n'))
                    decoded = Image.open(io.BytesIO(body))
                    decoded.load()
                    self.assertEqual(decoded.size, (1, 1) if mode == 'empty' else (600, 600) if jpeg else (148, 148))
                    if mode == 'empty':
                        self.assertEqual(decoded.convert('RGBA').getpixel((0, 0))[3], 0)
                    elif jpeg:
                        error = sum(abs(a - b) for a, b in
                                    zip(decoded.convert('RGB').resize((148, 148), Image.BOX).tobytes(), image.tobytes()))
                        self.assertLess(error / (148 * 148 * 3), 5)
                    else:
                        self.assertEqual(decoded.convert('RGB').tobytes(), image.tobytes())


if __name__ == '__main__':
    unittest.main()
