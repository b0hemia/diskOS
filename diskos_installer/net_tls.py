# SPDX-License-Identifier: GPL-3.0-or-later
"""One verified TLS context and the exact-host URL allowlist for firmware fetches.

Trust model: the system default roots AND the pinned certifi bundle are both
loaded, so a machine without usable system roots (python.org macOS without its
certificate step, a bare frozen build) still verifies. Verification is never
switched off: CERT_REQUIRED, hostname checking and TLS 1.2 or newer, always.

SSL_CERT_FILE (an explicitly configured custom CA file, for example a company
proxy root) is honoured ADDITIVELY: its certificates are trusted besides the
two sets above. If it is set but missing, unreadable or not a certificate file,
building the context fails with E151; it is never silently ignored and never
replaces the pinned bundle. An empty value counts as unset.
"""
import os
import ssl
from urllib.parse import urlsplit

from . import errors

ALLOWED_HOSTS = frozenset(('fiio-firmware.fiio.net', 'fiio-firmware.oss-us-west-1.aliyuncs.com'))


class UrlNotAllowed(ValueError):
    """The URL is not an exact-host https:443 firmware address."""


def check_url(url):
    """Return `url` if it may be fetched, else raise UrlNotAllowed.

    Call it for the first URL and for every redirect target before following it.
    Only https, port 443 (explicit or implied) and the exact hostnames above
    pass: no userinfo, no suffix or subdomain, no trailing dot, no IP literal,
    no non-ASCII or control characters. Host case is ignored (DNS is case-blind).
    """
    if not isinstance(url, str) or not url:
        raise UrlNotAllowed('URL must be a non-empty string')
    if not url.isascii() or any(c <= ' ' or c in '\x7f\\' for c in url):
        raise UrlNotAllowed('URL has characters outside the plain ASCII allowlist')
    try:
        parts = urlsplit(url)
    except ValueError as exc:
        raise UrlNotAllowed('URL does not parse') from exc
    if parts.scheme != 'https':
        raise UrlNotAllowed('only https is allowed')
    host = parts.netloc.lower()
    if host.endswith(':443'):
        host = host[:-4]
    if host not in ALLOWED_HOSTS:
        raise UrlNotAllowed('host is not on the firmware allowlist')
    return url


def _certifi_bundle():
    try:
        import certifi
        path = certifi.where()
    except Exception as exc:  # ImportError, or a damaged frozen layout
        raise errors.PreflightError(
            'The CA bundle (certifi) is missing from this installation (%s)' % exc, code='E150',
            action='reinstall the installer, or re-run ./install.sh; a browser download still works') from exc
    if not os.path.isfile(path):
        raise errors.PreflightError(
            'The CA bundle file is missing: %s' % path, code='E150',
            action='reinstall the installer; a browser download still works')
    return path


def build_context(bundle=None, environ=None):
    """Return the verified client SSLContext. `bundle` and `environ` exist for
    tests only; production calls build_context() with no arguments."""
    env = os.environ if environ is None else environ
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.verify_mode = ssl.CERT_REQUIRED
    ctx.check_hostname = True
    ctx.minimum_version = ssl.TLSVersion.TLSv1_2
    ctx.load_default_certs()
    # load_default_certs() honours SSL_CERT_FILE / SSL_CERT_DIR, which replace the
    # default OpenSSL locations. Restore those defaults explicitly (without touching
    # the environment) so system roots, certifi and a custom file are all trusted.
    defaults = ssl.get_default_verify_paths()
    cafile = defaults.openssl_cafile
    capath = defaults.openssl_capath
    cafile = cafile if cafile and os.path.isfile(cafile) else None
    capath = capath if capath and os.path.isdir(capath) else None
    if cafile or capath:
        try:
            ctx.load_verify_locations(cafile=cafile, capath=capath)
        except (OSError, ssl.SSLError) as exc:
            raise errors.PreflightError(
                'System CA roots cannot be loaded (%s)' % exc,
                code='E150',
                action='repair this computer\'s certificate installation') from exc
    path = bundle or _certifi_bundle()
    try:
        ctx.load_verify_locations(cafile=path)
    except (OSError, ssl.SSLError) as exc:
        raise errors.PreflightError('The CA bundle cannot be loaded: %s (%s)' % (path, exc), code='E150',
                                    action='reinstall the installer; a browser download still works') from exc
    custom = env.get('SSL_CERT_FILE')
    if custom:
        try:
            ctx.load_verify_locations(cafile=custom)
        except (OSError, ssl.SSLError) as exc:
            raise errors.PreflightError(
                'SSL_CERT_FILE is set but unusable: %s (%s)' % (custom, exc), code='E151',
                action='fix or unset SSL_CERT_FILE') from exc
    return ctx
