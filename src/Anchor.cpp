#include "Anchor.h"
#include "Settings.h"

namespace
{
	std::atomic<bool> g_on{ true };

	void Notify(const char* a_text)
	{
		using func_t = void(const char*, const char*, bool);
		static REL::Relocation<func_t> func{ REL::RelocationID(52050, 52933) };
		func(a_text, nullptr, true);
	}

	class ToggleKey : public RE::BSTEventSink<RE::InputEvent*>
	{
	public:
		static ToggleKey* GetSingleton()
		{
			static ToggleKey instance;
			return &instance;
		}

		RE::BSEventNotifyControl ProcessEvent(RE::InputEvent* const* a_event, RE::BSTEventSource<RE::InputEvent*>*) override
		{
			const auto key = Settings::GetSingleton().toggleKey;
			for (auto* event = a_event ? *a_event : nullptr; event; event = event->next) {
				if (event->GetEventType() != RE::INPUT_EVENT_TYPE::kButton || event->GetDevice() != RE::INPUT_DEVICE::kKeyboard) {
					continue;
				}
				const auto* button = event->AsButtonEvent();
				if (!button || !button->IsDown() || button->GetIDCode() != key) {
					continue;
				}
				if (auto* ui = RE::UI::GetSingleton(); ui && ui->IsMenuOpen(RE::Console::MENU_NAME)) {
					continue;
				}
				const bool on = !g_on.load();
				g_on.store(on);
				Notify(on ? "Actor Anchored Sound: on" : "Actor Anchored Sound: off (vanilla)");
				logger::info("Toggle key: {}", on ? "on" : "off (vanilla)");
			}
			return RE::BSEventNotifyControl::kContinue;
		}
	};

	struct Vec3
	{
		float x, y, z;
	};

	struct Listener
	{
		Vec3  orientFront;
		Vec3  orientTop;
		Vec3  position;
		Vec3  velocity;
		void* cone;
	};

	struct Emitter
	{
		void*         cone;
		Vec3          orientFront;
		Vec3          orientTop;
		Vec3          position;
		Vec3          velocity;
		float         innerRadius;
		float         innerRadiusAngle;
		std::uint32_t channelCount;
		float         channelRadius;
		float*        channelAzimuths;
		void*         volumeCurve;
		void*         lfeCurve;
		void*         lpfDirectCurve;
		void*         lpfReverbCurve;
		void*         reverbCurve;
		float         curveDistanceScaler;
		float         dopplerScaler;
	};
	static_assert(offsetof(Emitter, position) == 0x20 && sizeof(Emitter) == 0x80);

	struct DSPSettings
	{
		float*        matrixCoefficients;
		float*        delayTimes;
		std::uint32_t srcChannelCount;
		std::uint32_t dstChannelCount;
		float         lpfDirect;
		float         lpfReverb;
		float         reverbLevel;
		float         dopplerFactor;
		float         emitterToListenerAngle;
		float         emitterToListenerDistance;
	};
	static_assert(offsetof(DSPSettings, emitterToListenerDistance) == 0x2C);

	constexpr std::ptrdiff_t kDSP = 0x130;
	constexpr std::ptrdiff_t kEmitter = 0x168;

	constexpr std::uint32_t kMatrix = 0x1, kLowPassDirect = 0x4, kLowPassReverb = 0x8, kReverb = 0x10;

	using Calculate_t = void (*)(const void*, const Listener*, const Emitter*, std::uint32_t, DSPSettings*);
	Calculate_t g_calculate = nullptr;
	Calculate_t g_x3daudio = nullptr;

	bool CharacterPoint(RE::NiPoint3& a_at)
	{
		auto* camera = RE::PlayerCamera::GetSingleton();
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!camera || !player || camera->IsInFirstPerson()) {
			return false;
		}
		static RE::NiPointer<RE::NiAVObject> root, head;
		RE::NiAVObject* const                now = player->Get3D(false);
		if (!now) {
			return false;
		}
		if (now != root.get()) {
			root = RE::NiPointer<RE::NiAVObject>(now);
			head = RE::NiPointer<RE::NiAVObject>(now->GetObjectByName("NPC Head [Head]"));
		}
		if (!head) {
			return false;
		}
		a_at = head->world.translate;
		return true;
	}

	void CalculateThunk(const void* a_instance, const Listener* a_listener, const Emitter* a_emitter, std::uint32_t a_flags, DSPSettings* a_dsp)
	{
		g_calculate(a_instance, a_listener, a_emitter, a_flags, a_dsp);
		if (!g_on.load(std::memory_order_relaxed)) {
			return;
		}
		if (!a_listener || !a_emitter || !a_dsp || !a_dsp->matrixCoefficients || !(a_flags & kMatrix)) {
			return;
		}
		const auto sound = reinterpret_cast<std::uintptr_t>(a_emitter) - kEmitter;
		if (reinterpret_cast<std::uintptr_t>(a_dsp) != sound + kDSP || a_dsp->srcChannelCount == 0 || a_dsp->srcChannelCount > 8 ||
			a_dsp->dstChannelCount == 0 || a_dsp->dstChannelCount > 16) {
			return;
		}
		RE::NiPoint3 body;
		if (!CharacterPoint(body)) {
			return;
		}
		const std::uint32_t count = a_dsp->srcChannelCount * a_dsp->dstChannelCount;
		const std::uint32_t flags = a_flags & (kMatrix | kLowPassDirect | kLowPassReverb | kReverb);
		float               cameraMatrix[8 * 16]{}, characterMatrix[8 * 16]{};
		DSPSettings         camera{}, character{};
		camera.matrixCoefficients = cameraMatrix;
		character.matrixCoefficients = characterMatrix;
		camera.srcChannelCount = character.srcChannelCount = a_dsp->srcChannelCount;
		camera.dstChannelCount = character.dstChannelCount = a_dsp->dstChannelCount;
		Listener moved = *a_listener;
		moved.position = { body.x, body.z, body.y };
		g_x3daudio(a_instance, a_listener, a_emitter, flags, &camera);
		g_x3daudio(a_instance, &moved, a_emitter, flags, &character);
		double cameraSum = 0.0, characterSum = 0.0;
		for (std::uint32_t i = 0; i < count; ++i) {
			cameraSum += std::fabs(cameraMatrix[i]);
			characterSum += std::fabs(characterMatrix[i]);
		}
		if (cameraSum < 1e-6 || characterSum >= cameraSum) {
			return;
		}
		const float scale = static_cast<float>(characterSum / cameraSum);
		for (std::uint32_t i = 0; i < count; ++i) {
			a_dsp->matrixCoefficients[i] *= scale;
		}
		if (a_flags & kLowPassDirect) {
			a_dsp->lpfDirect = character.lpfDirect;
		}
		if (a_flags & kLowPassReverb) {
			a_dsp->lpfReverb = character.lpfReverb;
		}
		if (a_flags & kReverb) {
			a_dsp->reverbLevel = character.reverbLevel;
		}
	}

	constexpr std::ptrdiff_t kPoint = kEmitter + offsetof(Emitter, position);

	float FlatDistance(std::uintptr_t a_sound, float a_camera)
	{
		float        used = a_camera;
		RE::NiPoint3 body;
		const bool   moved = g_on.load(std::memory_order_relaxed) && a_camera >= 1.0f && CharacterPoint(body);
		if (moved) {
			const auto* at = reinterpret_cast<const float*>(a_sound + kPoint);
			const float dx = at[0] - body.x, dy = at[1] - body.z, dz = at[2] - body.y;
			used = (std::max)(a_camera, std::sqrt(dx * dx + dy * dy + dz * dz));
		}
		return used;
	}

	void InstallFlat()
	{
		REL::Relocation<std::uintptr_t> site{ REL::RelocationID(66764, 68005), REL::VariantOffset(0x3FC, 0x3E3, 0) };
		const auto* code = reinterpret_cast<const std::uint8_t*>(site.address());
		const bool  match = code[-3] == 0x48 && code[-2] == 0x8B && (code[-1] == 0xCE || code[-1] == 0xCD) && code[0] == 0xE8 &&
		                   code[5] == 0x84 && code[6] == 0xC0 && code[7] == 0x0F && code[8] == 0x84 && code[13] == 0xF3 &&
		                   code[14] == 0x0F && code[15] == 0x10 && code[17] == 0x74;
		if (!match) {
			logger::error("2D distance: the game code is not the expected one, 2D sounds left as they are");
			return;
		}
		const std::uintptr_t check = site.address() + 5 + *reinterpret_cast<const std::int32_t*>(code + 1);
		std::uint8_t stub[] = {
			0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x89, 0xCB,
			0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xD0,
			0x84, 0xC0, 0x74, 0x17, 0x48, 0x89, 0xD9, 0x0F, 0x28, 0xCE,
			0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xD0,
			0x0F, 0x28, 0xF0, 0xB0, 0x01,
			0x48, 0x83, 0xC4, 0x20, 0x5B, 0xC3
		};
		static_assert(sizeof(stub) == 53);
		const auto flat = reinterpret_cast<std::uintptr_t>(&FlatDistance);
		std::memcpy(stub + 10, &check, 8);
		std::memcpy(stub + 32, &flat, 8);
		auto& trampoline = SKSE::GetTrampoline();
		auto* memory = trampoline.allocate(sizeof(stub));
		std::memcpy(memory, stub, sizeof(stub));
		trampoline.write_call<5>(site.address(), reinterpret_cast<std::uintptr_t>(memory));
		logger::info("Installed: 2D sounds that fade with distance measured from the character");
	}
}

void Anchor::Install()
{
	if (const auto module = GetModuleHandleW(L"X3DAudio1_7.dll")) {
		g_x3daudio = reinterpret_cast<Calculate_t>(GetProcAddress(module, "X3DAudioCalculate"));
	}
	if (!g_x3daudio) {
		logger::error("X3DAudio1_7.dll's X3DAudioCalculate not found, nothing installed");
		return;
	}
	const auto calculate = SKSE::PatchIAT(CalculateThunk, "X3DAudio1_7.dll", "X3DAudioCalculate");
	if (!calculate) {
		logger::error("X3DAudioCalculate import not found, nothing installed");
		return;
	}
	g_calculate = reinterpret_cast<Calculate_t>(calculate);
	logger::info("Installed: X3DAudioCalculate import; third-person 3D sounds measured from the character");
	InstallFlat();
}

void Anchor::InstallToggleKey()
{
	const auto key = Settings::GetSingleton().toggleKey;
	auto*      input = RE::BSInputDeviceManager::GetSingleton();
	if (key == 0 || !input) {
		return;
	}
	input->AddEventSink(ToggleKey::GetSingleton());
	logger::info("Toggle key {} installed", key);
}
