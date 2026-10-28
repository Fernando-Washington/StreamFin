/*
    Curl-backed stream reader for mpv (Switch only).

    On the Nintendo Switch, the ffmpeg build mpv links against has no TLS
    backend at all (verified via its pkg-config metadata: no mbedtls/openssl/
    gnutls anywhere) -- so mpv/ffmpeg cannot open an "https://" URL on its
    own, and every debrid/stream addon link is https. libcurl, on the other
    hand, DOES have working TLS on Switch (it goes through the console's own
    ssl service via libnx), and the app's own HTTP client already uses it
    successfully for the JSON API.

    This registers an mpv stream_cb protocol handler that serves "https" (and
    "http") reads through our own curl requests instead of ffmpeg's, so
    playback doesn't depend on the missing TLS backend at all.
*/
#pragma once

struct mpv_handle;

void registerCurlHttpsStream(mpv_handle* mpv);
