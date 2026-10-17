/* Production dock, using real Qt widgets, libobs scenes and grouped sources.
 * Only playback/file decoding and the frontend scene switch are intercepted. */
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QListWidget>
#include "test-runtime.h"
static QString config_root, save_folder;
static bool load_success;
static int switches, loads;
static obs_source_t *switched_scene;
static char *config_path(const char *file)
{
	return bstrdup(QDir(config_root).filePath(file).toUtf8().constData());
}
static void switch_scene(obs_source_t *scene)
{
	switches++;
	switched_scene = scene;
}
#undef obs_module_config_path
#define obs_module_config_path config_path
#define obs_frontend_set_current_scene switch_scene
#include "../src/sr-dock.cpp"
const char *obs_module_text(const char *key)
{
	return key;
}
char *sr_config_get_save_dir(void)
{
	return bstrdup(save_folder.toUtf8().constData());
}
void sr_config_set_save_dir(const char *path)
{
	save_folder = QString::fromUtf8(path);
}
bool sr_playback_play_file(obs_source_t *, const char *)
{
	loads++;
	return load_success;
}
bool sr_thumbnail_rgba(const char *, int, int, uint8_t **out)
{
	*out = nullptr;
	return false;
}
static void check(bool ok, const char *message)
{
	printf("%s: %s\n", ok ? "PASS" : "FAIL", message);
	fflush(stdout);
	if (!ok)
		std::exit(2);
}
static bool marked(const QString &path)
{
	QFile file(QDir(config_root).filePath("played.json"));
	if (!file.open(QIODevice::ReadOnly))
		return false;
	for (const QJsonValue &v : QJsonDocument::fromJson(file.readAll()).object()["played"].toArray())
		if (v.toObject()["path"] == path)
			return true;
	return false;
}
static const char *name(void *)
{
	return "Dock test replay";
}
static void *create(obs_data_t *, obs_source_t *s)
{
	return s;
}
static void destroy(void *) {}
int main(int argc, char **argv)
{
	QApplication app(argc, argv);
	check(argc == 2, "isolated dock directory provided");
	config_root = QString::fromUtf8(argv[1]);
	QString a = QDir(config_root).filePath("A"), b = QDir(config_root).filePath("B");
	QDir().mkpath(a);
	QDir().mkpath(b);
	QString file_a = QDir(a).filePath("a.mp4"), file_b = QDir(b).filePath("b.mp4");
	for (const QString &path : {file_a, file_b}) {
		QFile f(path);
		check(f.open(QIODevice::WriteOnly), "dock fixture created");
	}
	save_folder = a;
	{
		SrDock dock;
		dock.markPlayed(file_a);
	}
	check(marked(file_a), "played marker is persisted");
	save_folder = b;
	{
		SrDock dock;
	}
	check(marked(file_a), "switching folders preserves markers from another folder");
	QFile::remove(file_a);
	save_folder = a;
	{
		SrDock dock;
	}
	check(!marked(file_a), "refresh still prunes deleted files in the selected folder");
	check(obs_startup("en-US", nullptr, nullptr), "isolated dock OBS context starts");
	check(sr_test_video(), "dock scene graphics context starts");
	sr_scene_tracker_start();
	obs_source_info info = {};
	info.id = SR_PLAYBACK_ID;
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_ASYNC_VIDEO;
	info.get_name = name;
	info.create = create;
	info.destroy = destroy;
	obs_register_source(&info);
	obs_source_t *source = obs_source_create(info.id, "Grouped replay", nullptr, nullptr);
	save_folder = b;
	{
		SrDock dock;
		auto *list = dock.findChild<QListWidget *>();
		check(list && list->count() == 1, "dock lists the replay fixture");
		list->itemDoubleClicked(list->item(0));
		check(!loads && !switches && !marked(file_b),
		      "unroutable replay neither loads nor acquires a played marker");
		obs_scene_t *scene = obs_scene_create("Replay scene");
		obs_sceneitem_t *group = obs_scene_add_group(scene, "Replay group");
		obs_scene_t *group_scene = obs_sceneitem_group_get_scene(group);
		obs_scene_add(group_scene, source);
		obs_source_t *found = sceneContaining("Grouped replay");
		check(found == obs_scene_get_source(scene), "dock finds a playback source inside a group");
		obs_source_release(found);
		list->itemDoubleClicked(list->item(0));
		check(loads == 1 && !switches && !marked(file_b),
		      "failed playback loading does not switch scenes or mark a replay");
		load_success = true;
		list->itemDoubleClicked(list->item(0));
		check(loads == 2 && switches == 1 && switched_scene == obs_scene_get_source(scene) && marked(file_b),
		      "successful grouped replay launches and persists its played marker");
		obs_sceneitem_remove(group);
		obs_wait_for_destroy_queue();
		obs_scene_release(scene);
	}
	obs_source_release(source);
	obs_wait_for_destroy_queue();
	sr_scene_tracker_stop();
	obs_shutdown();
	return 0;
}
