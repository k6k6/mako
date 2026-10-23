notification_icons_test = executable(
	'notification-icons-test',
	files('notification-icons.c') + mako_test_sources + protocols_src,
	dependencies: mako_deps,
	include_directories: mako_inc,
)

test('notification-icons', notification_icons_test)
