/* Query the actual Vendor callback inside source creation, before libobs
 * assigns context.data. Deterministic reproduction of the initialization race. */
#include "../src/sr-integration.cpp"
#include <QCoreApplication>
const char *obs_module_text(const char *key)
{
	return key;
}
obs_module_t *obs_current_module(void)
{
	return nullptr;
}
char *sr_config_get_save_dir(void)
{
	return bstrdup("");
}
void sr_dock_mark_played(const char *) {}
static QJsonObject during_create;
static void check(bool ok, const char *message)
{
	printf("%s: %s\n", ok ? "PASS" : "FAIL", message);
	if (!ok)
		std::exit(2);
}
static const char *name(void *)
{
	return "Initializing filter";
}
static void *create(obs_data_t *, obs_source_t *source)
{
	char *id = sr_integration_register_capture(source, "550e8400-e29b-41d4-a716-446655440001");
	obs_data_t *input = obs_data_create(), *output = obs_data_create();
	obs_data_set_string(input, "capture_id", id);
	check(obs_obj_get_data(source) == nullptr, "query runs inside the context.data initialization window");
	request_callback(input, output, const_cast<char *>("GetCaptureSource"));
	during_create = from_obs(output);
	obs_data_release(input);
	obs_data_release(output);
	bfree(id);
	return source;
}
static void destroy(void *data)
{
	sr_integration_unregister_capture(static_cast<obs_source_t *>(data));
}
int main(int argc, char **argv)
{
	QCoreApplication app(argc, argv);
	check(obs_startup("en-US", nullptr, nullptr), "initialization test context starts");
	stopping = false;
	obs_source_info info = {};
	info.id = "initialization-regression";
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_ASYNC_VIDEO;
	info.get_name = name;
	info.create = create;
	info.destroy = destroy;
	obs_register_source(&info);
	obs_source_t *source = obs_source_create(info.id, "Initializing capture", nullptr, nullptr);
	check(during_create["status"] == "OFFLINE" &&
		      during_create["capture_id"] == "550e8400-e29b-41d4-a716-446655440001",
	      "Vendor health query safely reports the known filter during initialization");
	obs_source_release(source);
	obs_wait_for_destroy_queue();
	stopping = true;
	obs_shutdown();
	return 0;
}
