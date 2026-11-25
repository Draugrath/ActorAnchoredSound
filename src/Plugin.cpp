#include "PCH.h"
#include "Version.h"
#include "Settings.h"
#include "Anchor.h"

#include <spdlog/sinks/basic_file_sink.h>

void InitializeLog()
{
	auto path = logger::log_directory();
	if (!path) {
		return;
	}

	*path /= std::format("{}.log", Plugin::NAME);
	auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);
	auto log = std::make_shared<spdlog::logger>("global log", std::move(sink));

	log->set_level(spdlog::level::info);
	log->flush_on(spdlog::level::info);

	spdlog::set_default_logger(std::move(log));
	spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	InitializeLog();
	SKSE::InitInfo info{};
	info.trampoline = true;
	info.trampolineSize = 128;
	SKSE::Init(a_skse, info);
	logger::info("Loaded v{}.{}.{} | {} {}",
		Plugin::VERSION.major(),
		Plugin::VERSION.minor(),
		Plugin::VERSION.patch(),
		REL::Module::IsAE() ? "AE" : "SE",
		REL::Module::get().version().string("."));

	Settings::GetSingleton().Load();
	if (Settings::GetSingleton().enabled) {
		Anchor::Install();
		SKSE::GetMessagingInterface()->RegisterListener([](SKSE::MessagingInterface::Message* a_message) {
			if (a_message->type == SKSE::MessagingInterface::kInputLoaded) {
				Anchor::InstallToggleKey();
			}
		});
	}

	return true;
}
