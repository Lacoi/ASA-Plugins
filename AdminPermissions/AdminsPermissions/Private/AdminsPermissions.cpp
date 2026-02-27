#include "Permissions.h"
#include "Requests.h"

#include <ctime>
#include <fstream>
#include <unordered_map>

#include <shellapi.h>

#include "json.hpp"

#pragma comment(lib, "AsaApi.lib")
#pragma comment(lib, "Permissions.lib")

nlohmann::json config;

std::string GetConfigPath()
{
	return API::Tools::GetCurrentDir() + "/ArkApi/Plugins/AdminsPermissions/config.json";
}

FString GetText(const std::string& str)
{
	return FString(API::Tools::Utf8Decode(config["Messages"].value(str, "No message")).c_str());
}

std::string GetTimestampString()
{
	std::time_t now = std::time(nullptr);
	std::tm local_time{};
	localtime_s(&local_time, &now);

	char time_buffer[32]{};
	std::strftime(time_buffer, sizeof(time_buffer), "%Y-%m-%d %H:%M:%S", &local_time);
	return time_buffer;
}

std::string GetMapNameString()
{
	LPWSTR* argv;
	int argc;
	int i;
	FString param(L"-serverkey=");
	FString map_name = L"Unknown";

	AShooterGameMode* game_mode = AsaApi::GetApiUtils().GetShooterGameMode();
	if (game_mode)
		game_mode->GetMapName(&map_name);

	argv = CommandLineToArgvW(GetCommandLineW(), &argc);
	if (NULL != argv)
	{
		for (i = 0; i < argc; i++)
		{
			FString arg(argv[i]);
			if (arg.Contains(param))
			{
				if (arg.RemoveFromStart(param))
				{
					map_name = arg;
					break;
				}
			}
		}

		LocalFree(argv);
	}

	return API::Tools::Utf8Encode(*map_name);
}

std::string GetLocationString(AShooterPlayerController* player_controller)
{
	if (!player_controller)
		return "cheat spi 0 0 0";

	const FVector loc = player_controller->GetLocation();
	return fmt::format("cheat spi {:.2f} {:.2f} {:.2f}", loc.X, loc.Y, loc.Z);
}

void SendWebhook(AShooterPlayerController* player_controller, const FString& command)
{
	const auto discord = config.value("DiscordWebhook", nlohmann::json::object());
	if (!discord.value("Enabled", false))
		return;

	const std::string webhook_url = discord.value("URL", "");
	if (webhook_url.empty())
	{
		Log::GetLog()->warn("DiscordWebhook.Enabled is true but DiscordWebhook.URL is empty");
		return;
	}

	const std::string webhook_template = discord.value(
		"Message",
		"[{timestamp}] {charactername} ({eos_id}) used /cheat {command} on {map} at {location}");

	const FString character_name_f = AsaApi::IApiUtils::GetCharacterName(player_controller);
	const std::string character_name = API::Tools::Utf8Encode(*character_name_f);
	const std::string eos_id = player_controller ? player_controller->GetEOSId().ToString() : "";
	const std::string timestamp = GetTimestampString();
	const std::string map_name = GetMapNameString();
	const std::string location = GetLocationString(player_controller);
	const std::string command_text = API::Tools::Utf8Encode(*command);

	std::string message_content;
	try
	{
		message_content = fmt::format(
			webhook_template,
			fmt::arg("eos_id", eos_id),
			fmt::arg("timestamp", timestamp),
			fmt::arg("charactername", character_name),
			fmt::arg("map", map_name),
			fmt::arg("location", location),
			fmt::arg("command", command_text));
	}
	catch (const std::exception& error)
	{
		Log::GetLog()->warn("Invalid DiscordWebhook.Message format: {}", error.what());
		return;
	}

	nlohmann::json payload;
	payload["content"] = message_content;

	const std::string sender_name = discord.value("SenderName", "");
	if (!sender_name.empty())
		payload["username"] = sender_name;

	API::Requests::Get().CreatePostRequest(
		webhook_url,
		[](bool, std::string, std::unordered_map<std::string, std::string>) {},
		payload.dump(),
		"application/json");
}

void ConsoleCommand(APlayerController* player, FString command)
{
	AShooterPlayerController* SPC = static_cast<AShooterPlayerController*>(player);
	const bool is_admin = SPC->bIsAdmin()(), is_cheat = SPC->bCheatPlayer()();
	SPC->bIsAdmin() = true;
	SPC->bCheatPlayer() = true;

	FString result;
	player->ConsoleCommand(&result, &command, true);

	SPC->bIsAdmin() = is_admin;
	SPC->bCheatPlayer() = is_cheat;
}

void OnChatMessage(AShooterPlayerController* player_controller, FString* message, int, int)
{
	TArray<FString> parsed;
	message->ParseIntoArray(parsed, L" ", true);

	if (!parsed.IsValidIndex(1))
		return;

	const FString eos_id = player_controller->GetEOSId();

	if (Permissions::IsPlayerHasPermission(eos_id, "Cheat." + parsed[1]))
	{
		if (!message->RemoveFromStart("/cheat "))
			return;

		const FString command = *message;
		ConsoleCommand(player_controller, command);

		FString log = FString::Format(*GetText("LogMsg"), *AsaApi::IApiUtils::GetCharacterName(player_controller),
			*command);

		const bool print_to_chat = config["PrintToChat"];
		if (print_to_chat)
			AsaApi::GetApiUtils().SendServerMessageToAll(FColorList::Yellow, *log);

		AsaApi::GetApiUtils().GetShooterGameMode()->PrintToGameplayLog(log);
		SendWebhook(player_controller, command);
	}
	else
	{
		AsaApi::GetApiUtils().SendChatMessage(player_controller, *GetText("Sender"), *GetText("NoPermissions"));
	}
}

void ReadConfig()
{
	const std::string config_path = GetConfigPath();
	std::ifstream file{ config_path };
	if (!file.is_open())
		throw std::runtime_error("Can't open config.json");

	file >> config;

	file.close();
}

void Load()
{
	Log::Get().Init("AdminsPermissions");

	try
	{
		ReadConfig();
	}
	catch (const std::exception& error)
	{
		Log::GetLog()->error(error.what());
		throw;
	}

	AsaApi::GetCommands().AddChatCommand("/cheat", &OnChatMessage);
}

void Unload()
{
	AsaApi::GetCommands().RemoveChatCommand("/cheat");
}

BOOL APIENTRY DllMain(HMODULE, DWORD ul_reason_for_call, LPVOID)
{
	switch (ul_reason_for_call)
	{
	case DLL_PROCESS_ATTACH:
		Load();
		break;
	case DLL_PROCESS_DETACH:
		Unload();
		break;
	}
	return TRUE;
}
