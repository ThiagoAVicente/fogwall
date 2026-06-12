project(
  'fogwall',
  'c',
  version: '0.1.0',
  license: 'MIT',
  default_options: [
    'c_std=c99',
    'warning_level=2',
  ],
)

cc = meson.get_compiler('c')

wayland_client = dependency('wayland-client')
wayland_egl = dependency('wayland-egl')
egl = dependency('egl')
glesv2 = dependency('glesv2')
libm = cc.find_library('m', required: false)

wayland_scanner = find_program('wayland-scanner')
python = find_program('python3')

protocol_xmls = [
  'protocols/wlr-layer-shell-unstable-v1.xml',
  'protocols/wlr-foreign-toplevel-management-unstable-v1.xml',
  'protocols/ext-foreign-toplevel-list-v1.xml',
  'protocols/xdg-shell.xml',
]

protocol_srcs = []
foreach xml : protocol_xmls
  protocol_srcs += custom_target(
    xml.underscorify() + '_code',
    input: xml,
    output: '@BASENAME@-protocol.c',
    command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
  )
  protocol_srcs += custom_target(
    xml.underscorify() + '_header',
    input: xml,
    output: '@BASENAME@-client-protocol.h',
    command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
  )
endforeach

embed = files('scripts/embed.py')
shader_hdrs = [
  custom_target(
    'fog_frag_h',
    input: 'src/shaders/fog.frag',
    output: 'fog_frag.h',
    command: [python, embed, 'fog_frag_src', '@INPUT@', '@OUTPUT@'],
  ),
  custom_target(
    'quad_vert_h',
    input: 'src/shaders/quad.vert',
    output: 'quad_vert.h',
    command: [python, embed, 'quad_vert_src', '@INPUT@', '@OUTPUT@'],
  ),
]

executable(
  'fogwall',
  [
    'src/main.c',
    'src/wayland.c',
    'src/egl.c',
    'src/shader.c',
    'src/hypr.c',
  ] + protocol_srcs + shader_hdrs,
  dependencies: [wayland_client, wayland_egl, egl, glesv2, libm],
  install: true,
)
