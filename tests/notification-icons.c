#include <cairo/cairo.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wayland-client.h>

#include "config.h"
#include "criteria.h"
#include "icon.h"
#include "mako.h"
#include "mode.h"
#include "notification.h"
#include "surface.h"
#include "types.h"
#include "wayland.h"

#ifdef HAVE_ICONS
#include <gdk-pixbuf/gdk-pixbuf.h>
#endif

static int failures = 0;

#define CHECK(cond) \
	do { \
		if (!(cond)) { \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			++failures; \
		} \
	} while (0)

#define CHECK_STR(actual, expected) \
	do { \
		const char *_a = (actual); \
		const char *_e = (expected); \
		if (_a == NULL || strcmp(_a, _e) != 0) { \
			fprintf(stderr, "FAIL %s:%d: expected \"%s\", got \"%s\"\n", \
				__FILE__, __LINE__, _e, _a ? _a : "(null)"); \
			++failures; \
		} \
	} while (0)

struct test_ctx {
	struct mako_state state;
	char *config_path;
};

static struct mako_surface *add_surface(struct mako_state *state,
		const char *output, enum zwlr_layer_shell_v1_layer layer,
		uint32_t anchor) {
	struct mako_surface *surface = calloc(1, sizeof(*surface));
	if (surface == NULL) {
		return NULL;
	}
	wl_list_init(&surface->link);
	surface->state = state;
	surface->configured_output = strdup(output);
	surface->layer = layer;
	surface->anchor = anchor;
	surface->scale = 1;
	wl_list_insert(&state->surfaces, &surface->link);
	return surface;
}

static char *write_config(const char *contents) {
	char template[] = "/tmp/mako-notif-icons-XXXXXX";
	int fd = mkstemp(template);
	if (fd < 0) {
		perror("mkstemp");
		return NULL;
	}
	FILE *f = fdopen(fd, "w");
	if (f == NULL) {
		close(fd);
		return NULL;
	}
	fputs(contents, f);
	fclose(f);
	return strdup(template);
}

static void init_ctx(struct test_ctx *ctx, const char *config) {
	memset(ctx, 0, sizeof(*ctx));

	struct mako_state *state = &ctx->state;
	wl_list_init(&state->outputs);
	wl_list_init(&state->seats);
	wl_list_init(&state->surfaces);
	wl_list_init(&state->notifications);
	wl_list_init(&state->history);
	wl_array_init(&state->current_modes);

	init_default_config(&state->config);

	ctx->config_path = write_config(config);
	CHECK(ctx->config_path != NULL);
	if (ctx->config_path != NULL) {
		CHECK(load_config_file(&state->config, strdup(ctx->config_path)) == 0);
	}

	// A surface matching the default style, so that apply_each_criteria does
	// not need to create a new one (create_surface itself needs no compositor,
	// but matching also exercises the output/anchor/layer lookup).
	CHECK(add_surface(state, "", ZWLR_LAYER_SHELL_V1_LAYER_TOP,
		ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
		ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT) != NULL);
}

static void finish_ctx(struct test_ctx *ctx) {
	struct mako_state *state = &ctx->state;

	struct mako_notification *notif, *tmp;
	wl_list_for_each_safe(notif, tmp, &state->notifications, link) {
		wl_list_remove(&notif->link);
		wl_list_init(&notif->link);
		destroy_notification(notif);
	}

	struct mako_surface *surface, *surface_tmp;
	wl_list_for_each_safe(surface, surface_tmp, &state->surfaces, link) {
		wl_list_remove(&surface->link);
		free(surface->configured_output);
		free(surface);
	}

	char **mode_ptr;
	wl_array_for_each(mode_ptr, &state->current_modes) {
		free(*mode_ptr);
	}
	wl_array_release(&state->current_modes);

	finish_config(&state->config);

	if (ctx->config_path != NULL) {
		unlink(ctx->config_path);
		free(ctx->config_path);
	}
}

static struct mako_notification *add_notification(struct test_ctx *ctx,
		const char *app_icon, const char *summary) {
	struct mako_notification *notif = create_notification(&ctx->state);
	CHECK(notif != NULL);
	if (notif == NULL) {
		return NULL;
	}
	free(notif->app_icon);
	notif->app_icon = strdup(app_icon);
	free(notif->summary);
	notif->summary = strdup(summary);
	wl_list_insert(&ctx->state.notifications, &notif->link);
	return notif;
}

// Emulate the style reset done by reapply_config()/restore before rematching.
static ssize_t rematch(struct test_ctx *ctx, struct mako_notification *notif) {
	finish_style(&notif->style);
	init_empty_style(&notif->style);
	return apply_each_criteria(&ctx->state.config.criteria, notif);
}

#ifdef HAVE_ICONS

static char *write_png(double r, double g, double b) {
	char template[] = "/tmp/mako-icon-XXXXXX";
	int fd = mkstemp(template);
	if (fd < 0) {
		perror("mkstemp");
		return NULL;
	}
	close(fd);

	cairo_surface_t *surface =
		cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 8, 8);
	cairo_t *cairo = cairo_create(surface);
	cairo_set_source_rgba(cairo, r, g, b, 1.0);
	cairo_paint(cairo);
	cairo_destroy(cairo);
	cairo_status_t status = cairo_surface_write_to_png(surface, template);
	cairo_surface_destroy(surface);
	if (status != CAIRO_STATUS_SUCCESS) {
		fprintf(stderr, "failed to write %s: %s\n", template,
			cairo_status_to_string(status));
		unlink(template);
		return NULL;
	}
	return strdup(template);
}

static bool icon_pixel(struct mako_icon *icon, int *r, int *g, int *b) {
	if (icon == NULL || icon->image == NULL) {
		return false;
	}
	cairo_surface_flush(icon->image);
	unsigned char *data = cairo_image_surface_get_data(icon->image);
	int width = cairo_image_surface_get_width(icon->image);
	int height = cairo_image_surface_get_height(icon->image);
	if (data == NULL || width == 0 || height == 0) {
		return false;
	}
	int stride = cairo_image_surface_get_stride(icon->image);
	unsigned char *pixel = data + stride * (height / 2) + 4 * (width / 2);
	*b = pixel[0];
	*g = pixel[1];
	*r = pixel[2];
	return true;
}

static void check_icon_color(struct mako_icon *icon, int r, int g, int b) {
	int actual_r = -1, actual_g = -1, actual_b = -1;
	CHECK(icon != NULL);
	if (!icon_pixel(icon, &actual_r, &actual_g, &actual_b)) {
		CHECK(false);
		return;
	}
	if (abs(actual_r - r) > 8 || abs(actual_g - g) > 8 ||
			abs(actual_b - b) > 8) {
		fprintf(stderr, "FAIL color: expected (%d,%d,%d), got (%d,%d,%d)\n",
			r, g, b, actual_r, actual_g, actual_b);
		++failures;
	}
}

#endif

// app-icon= and icon= are aliases, and the last matching rule wins.
static void test_alias_and_last_match_wins(void) {
	struct test_ctx ctx;
	init_ctx(&ctx,
		"app-icon=global\n"
		"[summary=alias]\n"
		"icon=first\n"
		"[summary=alias]\n"
		"app-icon=second\n");

	struct mako_notification *notif = add_notification(&ctx, "client", "alias");
	ssize_t matches = apply_each_criteria(&ctx.state.config.criteria, notif);
	CHECK(matches == 3);
	CHECK_STR(notif->app_icon, "second");
	CHECK_STR(notif->original_app_icon, "client");
	CHECK(notif->style.spec.app_icon);

	finish_ctx(&ctx);
}

// Every rule must match against the client-provided icon, not against an
// override produced by an earlier rule in the same pass, and repeated
// rematches must be stable.
static void test_rematch_matches_original(void) {
	struct test_ctx ctx;
	init_ctx(&ctx,
		"app-icon=global\n"
		"[app-icon=original]\n"
		"app-icon=one\n"
		"[app-icon=one]\n"
		"app-icon=chained\n");

	struct mako_notification *notif =
		add_notification(&ctx, "original", "anything");
	ssize_t matches = apply_each_criteria(&ctx.state.config.criteria, notif);
	CHECK(matches == 2);
	CHECK_STR(notif->app_icon, "one");
	CHECK_STR(notif->original_app_icon, "original");

	matches = apply_each_criteria(&ctx.state.config.criteria, notif);
	CHECK(matches == 2);
	CHECK_STR(notif->app_icon, "one");
	CHECK_STR(notif->original_app_icon, "original");

	finish_ctx(&ctx);
}

// An explicitly empty override and an invalid path must clear the icon. They
// must not fall back to the client-provided icon.
static void test_empty_and_invalid_no_fallback(void) {
#ifdef HAVE_ICONS
	char *client_png = write_png(1.0, 0.0, 0.0);
	CHECK(client_png != NULL);
#endif

	struct test_ctx ctx;
	init_ctx(&ctx,
		"[summary=empty]\n"
		"app-icon=\n"
		"[summary=invalid]\n"
		"app-icon=/nonexistent/mako-icon-nope.png\n");

#ifdef HAVE_ICONS
	struct mako_notification *empty =
		add_notification(&ctx, client_png, "empty");
	CHECK(apply_each_criteria(&ctx.state.config.criteria, empty) == 2);
	CHECK_STR(empty->app_icon, "");
	CHECK_STR(empty->original_app_icon, client_png);
	CHECK(empty->icon == NULL);

	struct mako_notification *invalid =
		add_notification(&ctx, client_png, "invalid");
	CHECK(apply_each_criteria(&ctx.state.config.criteria, invalid) == 2);
	CHECK_STR(invalid->app_icon, "/nonexistent/mako-icon-nope.png");
	CHECK_STR(invalid->original_app_icon, client_png);
	CHECK(invalid->icon == NULL);

	unlink(client_png);
	free(client_png);
#endif

	finish_ctx(&ctx);
}

// Removing a mode re-applies criteria; when the overriding rule no longer
// matches, both the icon name and the icon must be restored.
static void test_mode_removal_restores(void) {
#ifdef HAVE_ICONS
	char *client_png = write_png(1.0, 0.0, 0.0);
	char *override_png = write_png(0.0, 0.0, 1.0);
	CHECK(client_png != NULL);
	CHECK(override_png != NULL);
	char config[1024];
	snprintf(config, sizeof(config),
		"[summary=mode mode=special]\n"
		"app-icon=%s\n", override_png);
#else
	char config[] = "[summary=mode mode=special]\napp-icon=override\n";
#endif

	struct test_ctx ctx;
	init_ctx(&ctx, config);

#ifdef HAVE_ICONS
	struct mako_notification *notif =
		add_notification(&ctx, client_png, "mode");
#else
	struct mako_notification *notif =
		add_notification(&ctx, "client", "mode");
#endif

	char **slot = wl_array_add(&ctx.state.current_modes, sizeof(char *));
	*slot = strdup("special");
	CHECK(rematch(&ctx, notif) > 0);
#ifdef HAVE_ICONS
	CHECK_STR(notif->app_icon, override_png);
	CHECK_STR(notif->original_app_icon, client_png);
	check_icon_color(notif->icon, 0, 0, 255);
#else
	CHECK_STR(notif->app_icon, "override");
	CHECK_STR(notif->original_app_icon, "client");
	CHECK(notif->icon == NULL);
#endif

	// Drop the mode.
	char **mode_ptr;
	wl_array_for_each(mode_ptr, &ctx.state.current_modes) {
		free(*mode_ptr);
	}
	ctx.state.current_modes.size = 0;

	CHECK(rematch(&ctx, notif) > 0);
	CHECK(notif->original_app_icon == NULL);
#ifdef HAVE_ICONS
	CHECK_STR(notif->app_icon, client_png);
	check_icon_color(notif->icon, 255, 0, 0);
	unlink(override_png);
	free(override_png);
	unlink(client_png);
	free(client_png);
#else
	CHECK_STR(notif->app_icon, "client");
#endif

	finish_ctx(&ctx);
}

// Reloading a config that no longer overrides the icon restores the
// client-provided value.
static void test_reload_restores(void) {
#ifdef HAVE_ICONS
	char *client_png = write_png(1.0, 0.0, 0.0);
	char *override_png = write_png(0.0, 1.0, 0.0);
	CHECK(client_png != NULL);
	CHECK(override_png != NULL);
	char config[1024];
	snprintf(config, sizeof(config), "app-icon=%s\n", override_png);
#else
	char config[] = "app-icon=override\n";
#endif

	struct test_ctx ctx;
	init_ctx(&ctx, config);

#ifdef HAVE_ICONS
	struct mako_notification *notif =
		add_notification(&ctx, client_png, "reload");
#else
	struct mako_notification *notif =
		add_notification(&ctx, "client", "reload");
#endif
	CHECK(apply_each_criteria(&ctx.state.config.criteria, notif) > 0);
#ifdef HAVE_ICONS
	CHECK_STR(notif->app_icon, override_png);
#else
	CHECK_STR(notif->app_icon, "override");
#endif

	char *new_path = write_config("\n");
	CHECK(new_path != NULL);
	char *argv[] = { "mako", "-c", new_path, NULL };
	CHECK(reload_config(&ctx.state.config, 3, argv) == 0);

	CHECK(rematch(&ctx, notif) > 0);
	CHECK(notif->original_app_icon == NULL);
#ifdef HAVE_ICONS
	CHECK_STR(notif->app_icon, client_png);
	check_icon_color(notif->icon, 255, 0, 0);
	unlink(override_png);
	free(override_png);
	unlink(client_png);
	free(client_png);
#else
	CHECK_STR(notif->app_icon, "client");
#endif

	unlink(new_path);
	free(new_path);
	finish_ctx(&ctx);
}

// A rule-provided icon takes precedence over image-data; removing the rule
// restores the original image-data.
static void test_image_data_override_and_restore(void) {
#ifdef HAVE_ICONS
	char *override_png = write_png(1.0, 0.0, 0.0);
	CHECK(override_png != NULL);
	char config[1024];
	snprintf(config, sizeof(config),
		"[summary=data]\n"
		"app-icon=%s\n", override_png);
#else
	char config[] = "[summary=data]\napp-icon=override\n";
#endif

	struct test_ctx ctx;
	init_ctx(&ctx, config);

	struct mako_notification *notif = add_notification(&ctx, "", "data");

#ifdef HAVE_ICONS
	// 8x8 opaque green image-data.
	const int32_t width = 8, height = 8;
	struct mako_image_data *image_data = calloc(1, sizeof(*image_data));
	CHECK(image_data != NULL);
	image_data->width = width;
	image_data->height = height;
	image_data->rowstride = width * 4;
	image_data->has_alpha = true;
	image_data->bits_per_sample = 8;
	image_data->channels = 4;
	image_data->data = malloc((size_t)height * (size_t)image_data->rowstride);
	CHECK(image_data->data != NULL);
	for (int32_t y = 0; y < height; ++y) {
		for (int32_t x = 0; x < width; ++x) {
			unsigned char *pixel = image_data->data +
				(size_t)y * (size_t)image_data->rowstride + (size_t)x * 4;
			pixel[0] = 0;
			pixel[1] = 255;
			pixel[2] = 0;
			pixel[3] = 255;
		}
	}
	notif->image_data = image_data;

	CHECK(apply_each_criteria(&ctx.state.config.criteria, notif) > 0);
	CHECK_STR(notif->app_icon, override_png);
	CHECK(notif->image_data == image_data);
	check_icon_color(notif->icon, 255, 0, 0);

	// Reload without the override: image-data must be used again.
	char *new_path = write_config("\n");
	CHECK(new_path != NULL);
	char *argv[] = { "mako", "-c", new_path, NULL };
	CHECK(reload_config(&ctx.state.config, 3, argv) == 0);
	CHECK(rematch(&ctx, notif) > 0);
	CHECK_STR(notif->app_icon, "");
	CHECK(notif->image_data == image_data);
	check_icon_color(notif->icon, 0, 255, 0);

	unlink(new_path);
	free(new_path);
	unlink(override_png);
	free(override_png);
#else
	CHECK(apply_each_criteria(&ctx.state.config.criteria, notif) > 0);
	CHECK(notif->image_data == NULL);
#endif

	finish_ctx(&ctx);
}

// Resetting a notification clears an override and it can be applied again.
static void test_reset_then_reapply(void) {
#ifdef HAVE_ICONS
	char *png = write_png(1.0, 0.0, 0.0);
	CHECK(png != NULL);
	char config[1024];
	snprintf(config, sizeof(config),
		"[summary=reset]\n"
		"app-icon=%s\n", png);
#else
	char config[] = "[summary=reset]\napp-icon=override\n";
#endif

	struct test_ctx ctx;
	init_ctx(&ctx, config);

	struct mako_notification *notif =
		add_notification(&ctx, "client", "reset");
	CHECK(apply_each_criteria(&ctx.state.config.criteria, notif) > 0);
	CHECK(notif->original_app_icon != NULL);

	reset_notification(notif);
	CHECK_STR(notif->app_icon, "");
	CHECK(notif->original_app_icon == NULL);
	CHECK(notif->icon == NULL);

	free(notif->app_icon);
	notif->app_icon = strdup("client");
	free(notif->summary);
	notif->summary = strdup("other");
	CHECK(apply_each_criteria(&ctx.state.config.criteria, notif) > 0);
	CHECK_STR(notif->app_icon, "client");
	CHECK(notif->original_app_icon == NULL);

	free(notif->summary);
	notif->summary = strdup("reset");
	CHECK(apply_each_criteria(&ctx.state.config.criteria, notif) > 0);
#ifdef HAVE_ICONS
	CHECK_STR(notif->app_icon, png);
	CHECK_STR(notif->original_app_icon, "client");
	check_icon_color(notif->icon, 255, 0, 0);
	unlink(png);
	free(png);
#else
	CHECK_STR(notif->app_icon, "override");
	CHECK_STR(notif->original_app_icon, "client");
	CHECK(notif->icon == NULL);
#endif

	finish_ctx(&ctx);
}

// Theme names resolve through icon-path, and file:// URIs work as overrides.
static void test_theme_name_and_file_uri(void) {
	struct test_ctx ctx;
#ifdef HAVE_ICONS
	char *theme_png = write_png(1.0, 0.0, 0.0);
	char *uri_png = write_png(0.0, 0.0, 1.0);
	CHECK(theme_png != NULL);
	CHECK(uri_png != NULL);

	char theme_dir[] = "/tmp/mako-icon-theme-XXXXXX";
	CHECK(mkdtemp(theme_dir) != NULL);
	char subdir[512], installed[768];
	snprintf(subdir, sizeof(subdir), "%s/48x48", theme_dir);
	mkdir(subdir, 0755);
	snprintf(subdir, sizeof(subdir), "%s/48x48/apps", theme_dir);
	mkdir(subdir, 0755);
	snprintf(installed, sizeof(installed), "%s/48x48/apps/mako-theme-test.png",
		theme_dir);
	CHECK(rename(theme_png, installed) == 0);

	char uri[1200], config[2048];
	snprintf(uri, sizeof(uri), "file://%s", uri_png);
	snprintf(config, sizeof(config),
		"icon-path=%s\n"
		"[summary=theme]\n"
		"app-icon=mako-theme-test\n"
		"[summary=uri]\n"
		"app-icon=%s\n", theme_dir, uri);

	init_ctx(&ctx, config);

	struct mako_notification *theme =
		add_notification(&ctx, "", "theme");
	CHECK(apply_each_criteria(&ctx.state.config.criteria, theme) > 0);
	CHECK_STR(theme->app_icon, "mako-theme-test");
	CHECK(theme->icon != NULL);
	check_icon_color(theme->icon, 255, 0, 0);

	struct mako_notification *file_uri =
		add_notification(&ctx, "", "uri");
	CHECK(apply_each_criteria(&ctx.state.config.criteria, file_uri) > 0);
	CHECK_STR(file_uri->app_icon, uri);
	CHECK(file_uri->icon != NULL);
	check_icon_color(file_uri->icon, 0, 0, 255);

	unlink(installed);
	rmdir(subdir);
	snprintf(subdir, sizeof(subdir), "%s/48x48", theme_dir);
	rmdir(subdir);
	rmdir(theme_dir);
	free(theme_png);
	unlink(uri_png);
	free(uri_png);
#else
	init_ctx(&ctx, "\n");
#endif

	finish_ctx(&ctx);
}

int main(void) {
	test_alias_and_last_match_wins();
	test_rematch_matches_original();
	test_empty_and_invalid_no_fallback();
	test_mode_removal_restores();
	test_reload_restores();
	test_image_data_override_and_restore();
	test_reset_then_reapply();
	test_theme_name_and_file_uri();

	if (failures != 0) {
		fprintf(stderr, "%d notification icon test(s) failed\n", failures);
		return 1;
	}
	printf("notification icon tests passed\n");
	return 0;
}
