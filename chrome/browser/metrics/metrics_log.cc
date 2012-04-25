// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/metrics/metrics_log.h"

#include <string>
#include <vector>

#include "base/basictypes.h"
#include "base/file_util.h"
#include "base/lazy_instance.h"
#include "base/memory/scoped_ptr.h"
#include "base/perftimer.h"
#include "base/string_number_conversions.h"
#include "base/string_util.h"
#include "base/sys_info.h"
#include "base/third_party/nspr/prtime.h"
#include "base/time.h"
#include "base/utf_string_conversions.h"
#include "chrome/browser/autocomplete/autocomplete.h"
#include "chrome/browser/autocomplete/autocomplete_match.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/plugin_prefs.h"
#include "chrome/browser/prefs/pref_service.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "chrome/common/chrome_version_info.h"
#include "chrome/common/logging_chrome.h"
#include "chrome/common/metrics/proto/omnibox_event.pb.h"
#include "chrome/common/metrics/proto/system_profile.pb.h"
#include "chrome/common/pref_names.h"
#include "content/public/browser/content_browser_client.h"
#include "content/public/browser/gpu_data_manager.h"
#include "content/public/common/gpu_info.h"
#include "content/public/common/content_client.h"
#include "googleurl/src/gurl.h"
#include "ui/gfx/screen.h"
#include "webkit/plugins/webplugininfo.h"

#define OPEN_ELEMENT_FOR_SCOPE(name) ScopedElement scoped_element(this, name)

// http://blogs.msdn.com/oldnewthing/archive/2004/10/25/247180.aspx
#if defined(OS_WIN)
extern "C" IMAGE_DOS_HEADER __ImageBase;
#endif

using content::GpuDataManager;
using metrics::OmniboxEventProto;
using metrics::SystemProfileProto;
typedef base::FieldTrial::NameGroupId NameGroupId;

namespace {

// Returns the date at which the current metrics client ID was created as
// a string containing milliseconds since the epoch, or "0" if none was found.
std::string GetInstallDate(PrefService* pref) {
  if (!pref) {
    NOTREACHED();
    return "0";
  }

  return pref->GetString(prefs::kMetricsClientIDTimestamp);
}

OmniboxEventProto::InputType AsOmniboxEventInputType(
    AutocompleteInput::Type type) {
  switch (type) {
    case AutocompleteInput::INVALID:
      return OmniboxEventProto::INVALID;
    case AutocompleteInput::UNKNOWN:
      return OmniboxEventProto::UNKNOWN;
    case AutocompleteInput::REQUESTED_URL:
      return OmniboxEventProto::REQUESTED_URL;
    case AutocompleteInput::URL:
      return OmniboxEventProto::URL;
    case AutocompleteInput::QUERY:
      return OmniboxEventProto::QUERY;
    case AutocompleteInput::FORCED_QUERY:
      return OmniboxEventProto::FORCED_QUERY;
    default:
      NOTREACHED();
      return OmniboxEventProto::INVALID;
  }
}

OmniboxEventProto::Suggestion::ProviderType AsOmniboxEventProviderType(
    const AutocompleteProvider* provider) {
  if (!provider)
    return OmniboxEventProto::Suggestion::UNKNOWN_PROVIDER;

  const std::string& name = provider->name();
  if (name == "HistoryURL")
    return OmniboxEventProto::Suggestion::URL;
  if (name == "HistoryContents")
    return OmniboxEventProto::Suggestion::HISTORY_CONTENTS;
  if (name == "HistoryQuickProvider")
    return OmniboxEventProto::Suggestion::HISTORY_QUICK;
  if (name == "Search")
    return OmniboxEventProto::Suggestion::SEARCH;
  if (name == "Keyword")
    return OmniboxEventProto::Suggestion::KEYWORD;
  if (name == "Builtin")
    return OmniboxEventProto::Suggestion::BUILTIN;
  if (name == "ShortcutsProvider")
    return OmniboxEventProto::Suggestion::SHORTCUTS;
  if (name == "ExtensionApps")
    return OmniboxEventProto::Suggestion::EXTENSION_APPS;

  NOTREACHED();
  return OmniboxEventProto::Suggestion::UNKNOWN_PROVIDER;
}

OmniboxEventProto::Suggestion::ResultType AsOmniboxEventResultType(
    AutocompleteMatch::Type type) {
  switch (type) {
    case AutocompleteMatch::URL_WHAT_YOU_TYPED:
      return OmniboxEventProto::Suggestion::URL_WHAT_YOU_TYPED;
    case AutocompleteMatch::HISTORY_URL:
      return OmniboxEventProto::Suggestion::HISTORY_URL;
    case AutocompleteMatch::HISTORY_TITLE:
      return OmniboxEventProto::Suggestion::HISTORY_TITLE;
    case AutocompleteMatch::HISTORY_BODY:
      return OmniboxEventProto::Suggestion::HISTORY_BODY;
    case AutocompleteMatch::HISTORY_KEYWORD:
      return OmniboxEventProto::Suggestion::HISTORY_KEYWORD;
    case AutocompleteMatch::NAVSUGGEST:
      return OmniboxEventProto::Suggestion::NAVSUGGEST;
    case AutocompleteMatch::SEARCH_WHAT_YOU_TYPED:
      return OmniboxEventProto::Suggestion::SEARCH_WHAT_YOU_TYPED;
    case AutocompleteMatch::SEARCH_HISTORY:
      return OmniboxEventProto::Suggestion::SEARCH_HISTORY;
    case AutocompleteMatch::SEARCH_SUGGEST:
      return OmniboxEventProto::Suggestion::SEARCH_SUGGEST;
    case AutocompleteMatch::SEARCH_OTHER_ENGINE:
      return OmniboxEventProto::Suggestion::SEARCH_OTHER_ENGINE;
    case AutocompleteMatch::EXTENSION_APP:
      return OmniboxEventProto::Suggestion::EXTENSION_APP;
    default:
      NOTREACHED();
      return OmniboxEventProto::Suggestion::UNKNOWN_RESULT_TYPE;
  }
}

// Returns the plugin preferences corresponding for this user, if available.
// If multiple user profiles are loaded, returns the preferences corresponding
// to an arbitrary one of the profiles.
PluginPrefs* GetPluginPrefs() {
  ProfileManager* profile_manager = g_browser_process->profile_manager();

  if (!profile_manager) {
    // The profile manager can be NULL when testing.
    return NULL;
  }

  std::vector<Profile*> profiles = profile_manager->GetLoadedProfiles();
  if (profiles.empty())
    return NULL;

  return PluginPrefs::GetForProfile(profiles.front());
}

// Fills |plugin| with the info contained in |plugin_info| and |plugin_prefs|.
void SetPluginInfo(const webkit::WebPluginInfo& plugin_info,
                   const PluginPrefs* plugin_prefs,
                   SystemProfileProto::Plugin* plugin) {
  plugin->set_name(UTF16ToUTF8(plugin_info.name));
  plugin->set_filename(plugin_info.path.BaseName().AsUTF8Unsafe());
  plugin->set_version(UTF16ToUTF8(plugin_info.version));
  if (plugin_prefs)
    plugin->set_is_disabled(!plugin_prefs->IsPluginEnabled(plugin_info));
}

void WriteFieldTrials(const std::vector<NameGroupId>& field_trial_ids,
                      SystemProfileProto* system_profile) {
  for (std::vector<NameGroupId>::const_iterator it = field_trial_ids.begin();
       it != field_trial_ids.end(); ++it) {
    SystemProfileProto::FieldTrial* field_trial =
        system_profile->add_field_trial();
    field_trial->set_name_id(it->name);
    field_trial->set_group_id(it->group);
  }
}

}  // namespace

static base::LazyInstance<std::string>::Leaky
  g_version_extension = LAZY_INSTANCE_INITIALIZER;

MetricsLog::MetricsLog(const std::string& client_id, int session_id)
    : MetricsLogBase(client_id, session_id, MetricsLog::GetVersionString()) {}

MetricsLog::~MetricsLog() {}

// static
void MetricsLog::RegisterPrefs(PrefService* local_state) {
  local_state->RegisterListPref(prefs::kStabilityPluginStats);
}

// static
int64 MetricsLog::GetIncrementalUptime(PrefService* pref) {
  base::TimeTicks now = base::TimeTicks::Now();
  static base::TimeTicks last_updated_time(now);
  int64 incremental_time = (now - last_updated_time).InSeconds();
  last_updated_time = now;

  if (incremental_time > 0) {
    int64 metrics_uptime = pref->GetInt64(prefs::kUninstallMetricsUptimeSec);
    metrics_uptime += incremental_time;
    pref->SetInt64(prefs::kUninstallMetricsUptimeSec, metrics_uptime);
  }

  return incremental_time;
}

// static
std::string MetricsLog::GetVersionString() {
  chrome::VersionInfo version_info;
  if (!version_info.is_valid()) {
    NOTREACHED() << "Unable to retrieve version info.";
    return std::string();
  }

  std::string version = version_info.Version();
  if (!version_extension().empty())
    version += version_extension();
  if (!version_info.IsOfficialBuild())
    version.append("-devel");
  return version;
}

// static
void MetricsLog::set_version_extension(const std::string& extension) {
  g_version_extension.Get() = extension;
}

// static
const std::string& MetricsLog::version_extension() {
  return g_version_extension.Get();
}

void MetricsLog::RecordIncrementalStabilityElements(
    const std::vector<webkit::WebPluginInfo>& plugin_list) {
  DCHECK(!locked());

  PrefService* pref = GetPrefService();
  DCHECK(pref);

  OPEN_ELEMENT_FOR_SCOPE("profile");
  WriteCommonEventAttributes();

  WriteInstallElement();

  {
    OPEN_ELEMENT_FOR_SCOPE("stability");  // Minimal set of stability elements.
    WriteRequiredStabilityAttributes(pref);
    WriteRealtimeStabilityAttributes(pref);

    WritePluginStabilityElements(plugin_list, pref);
  }
}

PrefService* MetricsLog::GetPrefService() {
  return g_browser_process->local_state();
}

gfx::Size MetricsLog::GetScreenSize() const {
  return gfx::Screen::GetPrimaryMonitor().size();
}

int MetricsLog::GetScreenCount() const {
  return gfx::Screen::GetNumMonitors();
}


void MetricsLog::GetFieldTrialIds(
    std::vector<NameGroupId>* field_trial_ids) const {
  base::FieldTrialList::GetFieldTrialNameGroupIds(field_trial_ids);
}

void MetricsLog::WriteStabilityElement(
    const std::vector<webkit::WebPluginInfo>& plugin_list,
    PrefService* pref) {
  DCHECK(!locked());

  // Get stability attributes out of Local State, zeroing out stored values.
  // NOTE: This could lead to some data loss if this report isn't successfully
  //       sent, but that's true for all the metrics.

  OPEN_ELEMENT_FOR_SCOPE("stability");
  WriteRequiredStabilityAttributes(pref);
  WriteRealtimeStabilityAttributes(pref);

  int incomplete_shutdown_count =
      pref->GetInteger(prefs::kStabilityIncompleteSessionEndCount);
  pref->SetInteger(prefs::kStabilityIncompleteSessionEndCount, 0);
  int breakpad_registration_success_count =
      pref->GetInteger(prefs::kStabilityBreakpadRegistrationSuccess);
  pref->SetInteger(prefs::kStabilityBreakpadRegistrationSuccess, 0);
  int breakpad_registration_failure_count =
      pref->GetInteger(prefs::kStabilityBreakpadRegistrationFail);
  pref->SetInteger(prefs::kStabilityBreakpadRegistrationFail, 0);
  int debugger_present_count =
      pref->GetInteger(prefs::kStabilityDebuggerPresent);
  pref->SetInteger(prefs::kStabilityDebuggerPresent, 0);
  int debugger_not_present_count =
      pref->GetInteger(prefs::kStabilityDebuggerNotPresent);
  pref->SetInteger(prefs::kStabilityDebuggerNotPresent, 0);

  // TODO(jar): The following are all optional, so we *could* optimize them for
  // values of zero (and not include them).

  // Write the XML version.
  WriteIntAttribute("incompleteshutdowncount", incomplete_shutdown_count);
  WriteIntAttribute("breakpadregistrationok",
                    breakpad_registration_success_count);
  WriteIntAttribute("breakpadregistrationfail",
                    breakpad_registration_failure_count);
  WriteIntAttribute("debuggerpresent", debugger_present_count);
  WriteIntAttribute("debuggernotpresent", debugger_not_present_count);

  // Write the protobuf version.
  SystemProfileProto::Stability* stability =
      uma_proto()->mutable_system_profile()->mutable_stability();
  stability->set_incomplete_shutdown_count(incomplete_shutdown_count);
  stability->set_breakpad_registration_success_count(
      breakpad_registration_success_count);
  stability->set_breakpad_registration_failure_count(
      breakpad_registration_failure_count);
  stability->set_debugger_present_count(debugger_present_count);
  stability->set_debugger_not_present_count(debugger_not_present_count);

  WritePluginStabilityElements(plugin_list, pref);
}

void MetricsLog::WritePluginStabilityElements(
    const std::vector<webkit::WebPluginInfo>& plugin_list,
    PrefService* pref) {
  // Now log plugin stability info.
  const ListValue* plugin_stats_list = pref->GetList(
      prefs::kStabilityPluginStats);
  if (!plugin_stats_list)
    return;

  OPEN_ELEMENT_FOR_SCOPE("plugins");
  SystemProfileProto::Stability* stability =
      uma_proto()->mutable_system_profile()->mutable_stability();
  PluginPrefs* plugin_prefs = GetPluginPrefs();
  for (ListValue::const_iterator iter = plugin_stats_list->begin();
       iter != plugin_stats_list->end(); ++iter) {
    if (!(*iter)->IsType(Value::TYPE_DICTIONARY)) {
      NOTREACHED();
      continue;
    }
    DictionaryValue* plugin_dict = static_cast<DictionaryValue*>(*iter);

    std::string plugin_name;
    plugin_dict->GetString(prefs::kStabilityPluginName, &plugin_name);

    std::string base64_name_hash;
    uint64 numeric_name_hash_ignored;
    CreateHashes(plugin_name, &base64_name_hash, &numeric_name_hash_ignored);

    // Write the XML verison.
    OPEN_ELEMENT_FOR_SCOPE("pluginstability");
    // Use "filename" instead of "name", otherwise we need to update the
    // UMA servers.
    WriteAttribute("filename", base64_name_hash);

    int launches = 0;
    plugin_dict->GetInteger(prefs::kStabilityPluginLaunches, &launches);
    WriteIntAttribute("launchcount", launches);

    int instances = 0;
    plugin_dict->GetInteger(prefs::kStabilityPluginInstances, &instances);
    WriteIntAttribute("instancecount", instances);

    int crashes = 0;
    plugin_dict->GetInteger(prefs::kStabilityPluginCrashes, &crashes);
    WriteIntAttribute("crashcount", crashes);

    // Write the protobuf version.
    // Note that this search is potentially a quadratic operation, but given the
    // low number of plugins installed on a "reasonable" setup, this should be
    // fine.
    // TODO(isherman): Verify that this does not show up as a hotspot in
    // profiler runs.
    const webkit::WebPluginInfo* plugin_info = NULL;
    const string16 plugin_name_utf16 = UTF8ToUTF16(plugin_name);
    for (std::vector<webkit::WebPluginInfo>::const_iterator iter =
             plugin_list.begin();
         iter != plugin_list.end(); ++iter) {
      if (iter->name == plugin_name_utf16) {
        plugin_info = &(*iter);
        break;
      }
    }

    if (!plugin_info) {
      NOTREACHED();
      continue;
    }

    SystemProfileProto::Stability::PluginStability* plugin_stability =
        stability->add_plugin_stability();
    SetPluginInfo(*plugin_info, plugin_prefs,
                  plugin_stability->mutable_plugin());
    plugin_stability->set_launch_count(launches);
    plugin_stability->set_instance_count(instances);
    plugin_stability->set_crash_count(crashes);
  }

  pref->ClearPref(prefs::kStabilityPluginStats);
}

// The server refuses data that doesn't have certain values.  crashcount and
// launchcount are currently "required" in the "stability" group.
// TODO(isherman): Stop writing these attributes specially once the migration to
// protobufs is complete.
void MetricsLog::WriteRequiredStabilityAttributes(PrefService* pref) {
  int launch_count = pref->GetInteger(prefs::kStabilityLaunchCount);
  pref->SetInteger(prefs::kStabilityLaunchCount, 0);
  int crash_count = pref->GetInteger(prefs::kStabilityCrashCount);
  pref->SetInteger(prefs::kStabilityCrashCount, 0);

  // Write the XML version.
  WriteIntAttribute("launchcount", launch_count);
  WriteIntAttribute("crashcount", crash_count);

  // Write the protobuf version.
  SystemProfileProto::Stability* stability =
      uma_proto()->mutable_system_profile()->mutable_stability();
  stability->set_launch_count(launch_count);
  stability->set_crash_count(crash_count);
}

void MetricsLog::WriteRealtimeStabilityAttributes(PrefService* pref) {
  // Update the stats which are critical for real-time stability monitoring.
  // Since these are "optional," only list ones that are non-zero, as the counts
  // are aggergated (summed) server side.

  SystemProfileProto::Stability* stability =
      uma_proto()->mutable_system_profile()->mutable_stability();
  int count = pref->GetInteger(prefs::kStabilityPageLoadCount);
  if (count) {
    WriteIntAttribute("pageloadcount", count);
    stability->set_page_load_count(count);
    pref->SetInteger(prefs::kStabilityPageLoadCount, 0);
  }

  count = pref->GetInteger(prefs::kStabilityRendererCrashCount);
  if (count) {
    WriteIntAttribute("renderercrashcount", count);
    stability->set_renderer_crash_count(count);
    pref->SetInteger(prefs::kStabilityRendererCrashCount, 0);
  }

  count = pref->GetInteger(prefs::kStabilityExtensionRendererCrashCount);
  if (count) {
    WriteIntAttribute("extensionrenderercrashcount", count);
    stability->set_extension_renderer_crash_count(count);
    pref->SetInteger(prefs::kStabilityExtensionRendererCrashCount, 0);
  }

  count = pref->GetInteger(prefs::kStabilityRendererHangCount);
  if (count) {
    WriteIntAttribute("rendererhangcount", count);
    stability->set_renderer_hang_count(count);
    pref->SetInteger(prefs::kStabilityRendererHangCount, 0);
  }

  count = pref->GetInteger(prefs::kStabilityChildProcessCrashCount);
  if (count) {
    WriteIntAttribute("childprocesscrashcount", count);
    stability->set_child_process_crash_count(count);
    pref->SetInteger(prefs::kStabilityChildProcessCrashCount, 0);
  }

#if defined(OS_CHROMEOS)
  count = pref->GetInteger(prefs::kStabilityOtherUserCrashCount);
  if (count) {
    stability->set_other_user_crash_count(count);
    pref->SetInteger(prefs::kStabilityOtherUserCrashCount, 0);
  }

  count = pref->GetInteger(prefs::kStabilityKernelCrashCount);
  if (count) {
    stability->set_kernel_crash_count(count);
    pref->SetInteger(prefs::kStabilityKernelCrashCount, 0);
  }

  count = pref->GetInteger(prefs::kStabilitySystemUncleanShutdownCount);
  if (count) {
    stability->set_unclean_system_shutdown_count(count);
    pref->SetInteger(prefs::kStabilitySystemUncleanShutdownCount, 0);
  }
#endif  // OS_CHROMEOS

  int64 recent_duration = GetIncrementalUptime(pref);
  if (recent_duration) {
    WriteInt64Attribute("uptimesec", recent_duration);
    stability->set_uptime_sec(recent_duration);
  }
}

void MetricsLog::WritePluginList(
    const std::vector<webkit::WebPluginInfo>& plugin_list,
    bool write_as_xml) {
  DCHECK(!locked());

  PluginPrefs* plugin_prefs = GetPluginPrefs();

  OPEN_ELEMENT_FOR_SCOPE("plugins");
  SystemProfileProto* system_profile = uma_proto()->mutable_system_profile();
  for (std::vector<webkit::WebPluginInfo>::const_iterator iter =
           plugin_list.begin();
       iter != plugin_list.end(); ++iter) {
    if (write_as_xml) {
      std::string base64_name_hash;
      uint64 numeric_hash_ignored;
      CreateHashes(UTF16ToUTF8(iter->name), &base64_name_hash,
                   &numeric_hash_ignored);

      std::string filename_bytes = iter->path.BaseName().AsUTF8Unsafe();
      std::string base64_filename_hash;
      CreateHashes(filename_bytes, &base64_filename_hash,
                   &numeric_hash_ignored);

      // Write the XML version.
      OPEN_ELEMENT_FOR_SCOPE("plugin");

      // Plugin name and filename are hashed for the privacy of those
      // testing unreleased new extensions.
      WriteAttribute("name", base64_name_hash);
      WriteAttribute("filename", base64_filename_hash);
      WriteAttribute("version", UTF16ToUTF8(iter->version));
      if (plugin_prefs)
        WriteIntAttribute("disabled", !plugin_prefs->IsPluginEnabled(*iter));
    } else {
      // Write the protobuf version.
      SystemProfileProto::Plugin* plugin = system_profile->add_plugin();
      SetPluginInfo(*iter, plugin_prefs, plugin);
    }
  }
}

void MetricsLog::WriteInstallElement() {
  // Write the XML version.
  // We'll write the protobuf version in RecordEnvironmentProto().
  OPEN_ELEMENT_FOR_SCOPE("install");
  WriteAttribute("installdate", GetInstallDate(GetPrefService()));
  WriteIntAttribute("buildid", 0);  // We're using appversion instead.
}

void MetricsLog::RecordEnvironment(
         const std::vector<webkit::WebPluginInfo>& plugin_list,
         const DictionaryValue* profile_metrics) {
  DCHECK(!locked());

  PrefService* pref = GetPrefService();

  OPEN_ELEMENT_FOR_SCOPE("profile");
  WriteCommonEventAttributes();

  WriteInstallElement();

  // Write the XML version.
  // We'll write the protobuf version in RecordEnvironmentProto().
  bool write_as_xml = true;
  WritePluginList(plugin_list, write_as_xml);

  WriteStabilityElement(plugin_list, pref);

  {
    // Write the XML version.
    // We'll write the protobuf version in RecordEnvironmentProto().
    OPEN_ELEMENT_FOR_SCOPE("cpu");
    WriteAttribute("arch", base::SysInfo::CPUArchitecture());
  }

  {
    // Write the XML version.
    // We'll write the protobuf version in RecordEnvironmentProto().
    OPEN_ELEMENT_FOR_SCOPE("memory");
    WriteIntAttribute("mb", base::SysInfo::AmountOfPhysicalMemoryMB());
#if defined(OS_WIN)
    WriteIntAttribute("dllbase", reinterpret_cast<int>(&__ImageBase));
#endif
  }

  {
    // Write the XML version.
    // We'll write the protobuf version in RecordEnvironmentProto().
    OPEN_ELEMENT_FOR_SCOPE("os");
    WriteAttribute("name", base::SysInfo::OperatingSystemName());
    WriteAttribute("version", base::SysInfo::OperatingSystemVersion());
  }

  {
    OPEN_ELEMENT_FOR_SCOPE("gpu");
    const content::GPUInfo& gpu_info =
        GpuDataManager::GetInstance()->GetGPUInfo();

    // Write the XML version.
    // We'll write the protobuf version in RecordEnvironmentProto().
    WriteIntAttribute("vendorid", gpu_info.vendor_id);
    WriteIntAttribute("deviceid", gpu_info.device_id);
  }

  {
    const gfx::Size display_size = GetScreenSize();

    // Write the XML version.
    // We'll write the protobuf version in RecordEnvironmentProto().
    OPEN_ELEMENT_FOR_SCOPE("display");
    WriteIntAttribute("xsize", display_size.width());
    WriteIntAttribute("ysize", display_size.height());
    WriteIntAttribute("screens", GetScreenCount());
  }

  {
    OPEN_ELEMENT_FOR_SCOPE("bookmarks");
    int num_bookmarks_on_bookmark_bar =
        pref->GetInteger(prefs::kNumBookmarksOnBookmarkBar);
    int num_folders_on_bookmark_bar =
        pref->GetInteger(prefs::kNumFoldersOnBookmarkBar);
    int num_bookmarks_in_other_bookmarks_folder =
        pref->GetInteger(prefs::kNumBookmarksInOtherBookmarkFolder);
    int num_folders_in_other_bookmarks_folder =
        pref->GetInteger(prefs::kNumFoldersInOtherBookmarkFolder);
    {
      OPEN_ELEMENT_FOR_SCOPE("bookmarklocation");
      WriteAttribute("name", "full-tree");
      WriteIntAttribute("foldercount",
          num_folders_on_bookmark_bar + num_folders_in_other_bookmarks_folder);
      WriteIntAttribute("itemcount",
          num_bookmarks_on_bookmark_bar +
          num_bookmarks_in_other_bookmarks_folder);
    }
    {
      OPEN_ELEMENT_FOR_SCOPE("bookmarklocation");
      WriteAttribute("name", "toolbar");
      WriteIntAttribute("foldercount", num_folders_on_bookmark_bar);
      WriteIntAttribute("itemcount", num_bookmarks_on_bookmark_bar);
    }
  }

  {
    OPEN_ELEMENT_FOR_SCOPE("keywords");
    WriteIntAttribute("count", pref->GetInteger(prefs::kNumKeywords));
  }

  if (profile_metrics)
    WriteAllProfilesMetrics(*profile_metrics);

  RecordEnvironmentProto(plugin_list);
}

void MetricsLog::RecordEnvironmentProto(
    const std::vector<webkit::WebPluginInfo>& plugin_list) {
  SystemProfileProto* system_profile = uma_proto()->mutable_system_profile();
  int install_date;
  bool success = base::StringToInt(GetInstallDate(GetPrefService()),
                                   &install_date);
  DCHECK(success);
  system_profile->set_install_date(install_date);

  system_profile->set_application_locale(
      content::GetContentClient()->browser()->GetApplicationLocale());

  SystemProfileProto::Hardware* hardware = system_profile->mutable_hardware();
  hardware->set_cpu_architecture(base::SysInfo::CPUArchitecture());
  hardware->set_system_ram_mb(base::SysInfo::AmountOfPhysicalMemoryMB());
#if defined(OS_WIN)
  hardware->set_dll_base(reinterpret_cast<uint64>(&__ImageBase));
#endif

  SystemProfileProto::OS* os = system_profile->mutable_os();
  os->set_name(base::SysInfo::OperatingSystemName());
  os->set_version(base::SysInfo::OperatingSystemVersion());

  const content::GPUInfo& gpu_info =
      GpuDataManager::GetInstance()->GetGPUInfo();
  SystemProfileProto::Hardware::Graphics* gpu = hardware->mutable_gpu();
  gpu->set_vendor_id(gpu_info.vendor_id);
  gpu->set_device_id(gpu_info.device_id);
  gpu->set_driver_version(gpu_info.driver_version);
  gpu->set_driver_date(gpu_info.driver_date);
  SystemProfileProto::Hardware::Graphics::PerformanceStatistics*
      gpu_performance = gpu->mutable_performance_statistics();
  gpu_performance->set_graphics_score(gpu_info.performance_stats.graphics);
  gpu_performance->set_gaming_score(gpu_info.performance_stats.gaming);
  gpu_performance->set_overall_score(gpu_info.performance_stats.overall);

  const gfx::Size display_size = GetScreenSize();
  hardware->set_primary_screen_width(display_size.width());
  hardware->set_primary_screen_height(display_size.height());
  hardware->set_screen_count(GetScreenCount());

  bool write_as_xml = false;
  WritePluginList(plugin_list, write_as_xml);

  std::vector<NameGroupId> field_trial_ids;
  GetFieldTrialIds(&field_trial_ids);
  WriteFieldTrials(field_trial_ids, system_profile);
}

void MetricsLog::WriteAllProfilesMetrics(
    const DictionaryValue& all_profiles_metrics) {
  const std::string profile_prefix(prefs::kProfilePrefix);
  for (DictionaryValue::key_iterator i = all_profiles_metrics.begin_keys();
       i != all_profiles_metrics.end_keys(); ++i) {
    const std::string& key_name = *i;
    if (key_name.compare(0, profile_prefix.size(), profile_prefix) == 0) {
      DictionaryValue* profile;
      if (all_profiles_metrics.GetDictionaryWithoutPathExpansion(key_name,
                                                                 &profile))
        WriteProfileMetrics(key_name.substr(profile_prefix.size()), *profile);
    }
  }
}

void MetricsLog::WriteProfileMetrics(const std::string& profileidhash,
                                     const DictionaryValue& profile_metrics) {
  OPEN_ELEMENT_FOR_SCOPE("userprofile");
  WriteAttribute("profileidhash", profileidhash);
  for (DictionaryValue::key_iterator i = profile_metrics.begin_keys();
       i != profile_metrics.end_keys(); ++i) {
    Value* value;
    if (profile_metrics.GetWithoutPathExpansion(*i, &value)) {
      DCHECK(*i != "id");
      switch (value->GetType()) {
        case Value::TYPE_STRING: {
          std::string string_value;
          if (value->GetAsString(&string_value)) {
            OPEN_ELEMENT_FOR_SCOPE("profileparam");
            WriteAttribute("name", *i);
            WriteAttribute("value", string_value);
          }
          break;
        }

        case Value::TYPE_BOOLEAN: {
          bool bool_value;
          if (value->GetAsBoolean(&bool_value)) {
            OPEN_ELEMENT_FOR_SCOPE("profileparam");
            WriteAttribute("name", *i);
            WriteIntAttribute("value", bool_value ? 1 : 0);
          }
          break;
        }

        case Value::TYPE_INTEGER: {
          int int_value;
          if (value->GetAsInteger(&int_value)) {
            OPEN_ELEMENT_FOR_SCOPE("profileparam");
            WriteAttribute("name", *i);
            WriteIntAttribute("value", int_value);
          }
          break;
        }

        default:
          NOTREACHED();
          break;
      }
    }
  }
}

void MetricsLog::RecordOmniboxOpenedURL(const AutocompleteLog& log) {
  DCHECK(!locked());

  // Write the XML version.
  OPEN_ELEMENT_FOR_SCOPE("uielement");
  WriteAttribute("action", "autocomplete");
  WriteAttribute("targetidhash", "");
  // TODO(kochi): Properly track windows.
  WriteIntAttribute("window", 0);
  if (log.tab_id != -1) {
    // If we know what tab the autocomplete URL was opened in, log it.
    WriteIntAttribute("tab", static_cast<int>(log.tab_id));
  }
  WriteCommonEventAttributes();

  std::vector<string16> terms;
  const int num_terms =
      static_cast<int>(Tokenize(log.text, kWhitespaceUTF16, &terms));
  {
    OPEN_ELEMENT_FOR_SCOPE("autocomplete");

    WriteIntAttribute("typedlength", static_cast<int>(log.text.length()));
    WriteIntAttribute("numterms", num_terms);
    WriteIntAttribute("selectedindex", static_cast<int>(log.selected_index));
    WriteIntAttribute("completedlength",
                      static_cast<int>(log.inline_autocompleted_length));
    if (log.elapsed_time_since_user_first_modified_omnibox !=
        base::TimeDelta::FromMilliseconds(-1)) {
      // Only upload the typing duration if it is set/valid.
      WriteInt64Attribute("typingduration",
          log.elapsed_time_since_user_first_modified_omnibox.InMilliseconds());
    }
    const std::string input_type(
        AutocompleteInput::TypeToString(log.input_type));
    if (!input_type.empty())
      WriteAttribute("inputtype", input_type);

    for (AutocompleteResult::const_iterator i(log.result.begin());
         i != log.result.end(); ++i) {
      OPEN_ELEMENT_FOR_SCOPE("autocompleteitem");
      if (i->provider)
        WriteAttribute("provider", i->provider->name());
      const std::string result_type(AutocompleteMatch::TypeToString(i->type));
      if (!result_type.empty())
        WriteAttribute("resulttype", result_type);
      WriteIntAttribute("relevance", i->relevance);
      WriteIntAttribute("isstarred", i->starred ? 1 : 0);
    }
  }

  // Write the protobuf version.
  OmniboxEventProto* omnibox_event = uma_proto()->add_omnibox_event();
  omnibox_event->set_time(MetricsLogBase::GetCurrentTime());
  if (log.tab_id != -1) {
    // If we know what tab the autocomplete URL was opened in, log it.
    omnibox_event->set_tab_id(log.tab_id);
  }
  omnibox_event->set_typed_length(log.text.length());
  omnibox_event->set_num_typed_terms(num_terms);
  omnibox_event->set_selected_index(log.selected_index);
  omnibox_event->set_completed_length(log.inline_autocompleted_length);
  if (log.elapsed_time_since_user_first_modified_omnibox !=
      base::TimeDelta::FromMilliseconds(-1)) {
    // Only upload the typing duration if it is set/valid.
    omnibox_event->set_typing_duration_ms(
        log.elapsed_time_since_user_first_modified_omnibox.InMilliseconds());
  }
  omnibox_event->set_input_type(AsOmniboxEventInputType(log.input_type));
  for (AutocompleteResult::const_iterator i(log.result.begin());
       i != log.result.end(); ++i) {
    OmniboxEventProto::Suggestion* suggestion = omnibox_event->add_suggestion();
    suggestion->set_provider(AsOmniboxEventProviderType(i->provider));
    suggestion->set_result_type(AsOmniboxEventResultType(i->type));
    suggestion->set_relevance(i->relevance);
    suggestion->set_is_starred(i->starred);
  }

  ++num_events_;
}
