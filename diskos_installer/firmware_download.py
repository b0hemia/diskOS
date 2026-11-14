# SPDX-License-Identifier: GPL-3.0-or-later
"""Streaming download of one pinned FiiO firmware ZIP into the verified cache.

The bytes go straight into a cache-owned .part file in bounded chunks. This
module checks the length while receiving (never more than the pinned size plus
one byte is read) and then hands the file to firmware_catalogue.promote(), which
rehashes and publishes it atomically. Any failure or cancel removes the .part
file. An interrupted transfer always restarts from byte zero: no Range, no
resume, no state kept between attempts.

Redirect policy (every hop, before it is followed): the target must pass
net_tls.check_url (https, port 443, exact allowed host, no userinfo). Relative
Location values are resolved against the current URL first. A missing Location,
any other scheme or host, or more than MAX_HOPS redirects is refused; the
transport never falls back to plain http. Only HTTP 200 carries a body.

Cancellation: `cancel()` is polled before every hop and between reads, so a
cancel takes effect within one socket read. A read blocked on a silent peer is
bounded by `timeout` and by the overall `deadline`; a cancel that must also
interrupt DNS or a stalled connect belongs to the supervising helper (slice 4).
"""
import contextlib
import errno
import http.client
import os
import socket
import ssl
import time
import urllib.error
import urllib.request
from urllib.parse import urljoin

from . import errors, firmware_catalogue, net_tls

MAX_HOPS = 5
CHUNK = 1 << 18
_PROGRESS_STEP = 1 << 16
_open = open  # seam for the disk-full test


class DownloadError(errors.DiskOSError):
    code = 'E160'


class NetworkError(DownloadError):
    code = 'E161'


class Timeout(NetworkError):
    code = 'E162'


class DeadlineExceeded(NetworkError):
    code = 'E163'


class TlsError(DownloadError):
    code = 'E164'


class HttpStatusError(DownloadError):
    code = 'E165'

    def __init__(self, status):
        self.status = status
        super().__init__('FiiO answered HTTP %s' % status,
                         action='try again later, or download the ZIP in a browser')


class RedirectRefused(DownloadError):
    code = 'E166'


class SizeMismatch(DownloadError):
    code = 'E167'


class HashMismatch(DownloadError):
    code = 'E168'


class DiskFull(DownloadError):
    code = 'E169'


class Cancelled(DownloadError):
    code = 'E170'


class _NoRedirect(urllib.request.HTTPRedirectHandler):
    def http_error_302(self, req, fp, code, msg, headers):
        return None  # let HTTPDefaultErrorHandler return the raw HTTPError

    http_error_301 = http_error_303 = http_error_307 = http_error_308 = http_error_302


def _classify(exc):
    """Map a transport exception to a typed DownloadError."""
    reason = exc.reason if isinstance(exc, urllib.error.URLError) else exc
    if isinstance(reason, ssl.SSLError):
        return TlsError('Could not verify FiiO\'s secure connection (%s)' % reason,
                        action='check this computer\'s date, or use the browser download')
    if isinstance(reason, (socket.timeout, TimeoutError)):
        return Timeout('The connection timed out', action='check the connection and retry')
    if isinstance(reason, http.client.IncompleteRead):
        return SizeMismatch('The download ended early')
    return NetworkError('Network error: %s' % reason, action='check the connection and retry')


def _write_error(exc):
    if exc.errno in (errno.ENOSPC, getattr(errno, 'EDQUOT', errno.ENOSPC)):
        return DiskFull('Not enough space to save the firmware', action='free some disk space and retry')
    return DownloadError('Cannot write the firmware download: %s' % exc)


@contextlib.contextmanager
def _writing(out):
    try:
        with out:
            yield out
    except OSError as exc:
        raise _write_error(exc) from exc


def download(version, run_lock=None, progress=None, cancel=None, context=None,
             timeout=15.0, deadline=1800.0, clock=time.monotonic):
    """Download, verify and cache the pinned ZIP for `version`; return its path.

    progress(done, total): monotonic byte counts against the pinned size, ending
    at total. cancel(): return true to stop. Raises a DownloadError subclass;
    nothing is left behind on failure.
    """
    pin = firmware_catalogue._pin(version)
    started = clock()

    def check():
        if cancel is not None and cancel():
            raise Cancelled('Firmware download cancelled')
        if clock() - started > deadline:
            raise DeadlineExceeded('The download took too long', action='retry, or use the browser download')

    check()
    try:
        url = net_tls.check_url(pin.url)
    except net_tls.UrlNotAllowed as exc:
        raise RedirectRefused('Catalogue URL is not allowed: %s' % exc) from exc
    ctx = context if context is not None else net_tls.build_context()
    opener = urllib.request.build_opener(urllib.request.HTTPSHandler(context=ctx), _NoRedirect())
    response = None
    for _hop in range(MAX_HOPS + 1):
        check()
        request = urllib.request.Request(
            url, headers={'User-Agent': 'diskos-installer', 'Accept-Encoding': 'identity'})
        try:
            response = opener.open(request, timeout=max(0.001, min(timeout, deadline - (clock() - started))))
            break
        except urllib.error.HTTPError as exc:
            status, location = exc.code, exc.headers.get('Location')
            exc.close()
            if status not in (301, 302, 303, 307, 308):
                raise HttpStatusError(status) from None
            if not location:
                raise RedirectRefused('Redirect without a target') from None
            try:
                url = net_tls.check_url(urljoin(url, location))
            except ValueError as bad:
                raise RedirectRefused('Redirect refused: %s' % bad) from None
        except (urllib.error.URLError, OSError, http.client.HTTPException) as exc:
            raise _classify(exc) from exc
    else:
        raise RedirectRefused('Too many redirects')

    total = pin.size
    done = reported = 0
    partial = None
    published = False
    try:
        with response:
            if response.status != 200:
                raise HttpStatusError(response.status)
            length = response.headers.get('Content-Length')
            if length is not None and length.strip() != str(total):
                raise SizeMismatch('FiiO reports %s bytes, expected %d' % (length, total))
            try:
                partial = firmware_catalogue.new_partial(version, run_lock)
                out = _open(partial, 'wb')
            except OSError as exc:
                raise _write_error(exc) from exc
            with _writing(out):
                if progress:
                    progress(0, total)
                while True:
                    check()
                    try:
                        chunk = response.read1(min(CHUNK, total + 1 - done))
                    except (OSError, http.client.HTTPException) as exc:
                        raise _classify(exc) from exc
                    if not chunk:
                        break
                    done += len(chunk)
                    if done > total:
                        raise SizeMismatch('More data than the pinned %d bytes' % total)
                    try:
                        out.write(chunk)
                    except OSError as exc:
                        raise _write_error(exc) from exc
                    if progress and (done - reported >= _PROGRESS_STEP or done == total):
                        reported = done
                        progress(done, total)
            if done != total:
                raise SizeMismatch('The download ended early (%d of %d bytes)' % (done, total))
            try:
                path = firmware_catalogue.promote(
                    partial, version, run_lock, check=check)
            except ValueError as exc:
                raise HashMismatch('The download does not match the verified FiiO %s file' % version) from exc
            except OSError as exc:
                raise _write_error(exc) from exc
            published = True
            return path
    finally:
        if partial and not published:
            try:
                os.unlink(partial)
            except OSError:
                pass
