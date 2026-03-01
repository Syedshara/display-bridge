project('display-bridge-sender', 'c',
  version : '0.1.0',
  default_options : ['c_std=c11', 'warning_level=2'])

# Dependencies
dep_pipewire = dependency('libpipewire-0.3')
dep_va       = dependency('libva')
dep_va_drm   = dependency('libva-drm')
dep_srt      = dependency('srt')
dep_threads  = dependency('threads')
dep_gio      = dependency('gio-2.0')       # D-Bus portal (capture.c)
dep_glib     = dependency('glib-2.0')      # GLib types (capture.c)
dep_drm      = dependency('libdrm')        # DMA-BUF fourcc (encoder.c)
dep_avahi    = dependency('avahi-client')   # mDNS discovery (discovery.c)

inc = include_directories('include')

sources = [
  'src/main.c',
  'src/encoder.c',
  'src/capture.c',
  'src/streamer.c',
  'src/input.c',
  'src/discovery.c',
]

executable('db-sender',
  sources,
  include_directories : [inc, '../shared'],
  dependencies : [dep_pipewire, dep_va, dep_va_drm, dep_srt, dep_threads,
                   dep_gio, dep_glib, dep_drm, dep_avahi],
  install : true)

# Loopback smoke test (encoder + SRT, no PipeWire capture needed)
test_loopback = executable('test_loopback',
  ['../tests/test_loopback.c', 'src/encoder.c', 'src/streamer.c'],
  include_directories : [inc, '../shared'],
  dependencies : [dep_va, dep_va_drm, dep_srt, dep_threads, dep_drm],
  install : false)

# Encode-only test (no SRT, just VAAPI encode verification)
executable('test_encode_only',
  ['../tests/test_encode_only.c', 'src/encoder.c'],
  include_directories : [inc, '../shared'],
  dependencies : [dep_va, dep_va_drm, dep_threads, dep_drm],
  install : false)
