local platform_srcs

if OS == "windows" then
	platform_srcs = {
		"libs/glfw/src/win32_init.c", "libs/glfw/src/win32_monitor.c", "libs/glfw/src/win32_window.c",
		"libs/glfw/src/win32_joystick.c", "libs/glfw/src/win32_time.c", "libs/glfw/src/win32_thread.c",
		"libs/glfw/src/wgl_context.c", "libs/glfw/src/egl_context.c", "libs/glfw/src/osmesa_context.c",
	}

	obj_cxxflags( "libs/glfw/src/.*", "/D_GLFW_WIN32" )
	obj_cxxflags( "libs/glfw/src/.*", "/wd4152 /wd4204 /wd4244 /wd4456" )
elseif OS == "linux" then
	platform_srcs = {
		"libs/glfw/src/x11_init.c", "libs/glfw/src/x11_monitor.c", "libs/glfw/src/x11_window.c",
		"libs/glfw/src/xkb_unicode.c", "libs/glfw/src/linux_joystick.c", "libs/glfw/src/posix_time.c",
		"libs/glfw/src/posix_thread.c", "libs/glfw/src/glx_context.c", "libs/glfw/src/egl_context.c",
		"libs/glfw/src/osmesa_context.c",
	}

	obj_cxxflags( "libs/glfw/src/.*", "-D_GLFW_X11" )
end

lib( "glfw", {
	"libs/glfw/src/context.c", "libs/glfw/src/init.c", "libs/glfw/src/input.c",
	"libs/glfw/src/monitor.c", "libs/glfw/src/vulkan.c", "libs/glfw/src/window.c",
	platform_srcs
} )
