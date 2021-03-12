#include "TwitchBot.h"

extern "C"
{
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
#include "flite.h"
}

// FLITE functions
extern "C" cst_voice *register_cmu_us_kal();

inline void TextToSpeech(const char *str)
{
	cst_voice *v;
	flite_init();
	v = register_cmu_us_kal();
	flite_text_to_speech(str, v, "play");
}

// LUA functions
inline bool CheckLua(lua_State *L, int r)
{
	if (r != 0)
	{
		std::string errormsg = lua_tostring(L, -1);
		std::cerr << errormsg << std::endl;
		return false;
	}
	return true;
}

inline void LoadLuaString(lua_State *L, const char *varName, std::string &str)
{
	lua_getglobal(L, varName);
	if (lua_isstring(L, -1))
		str = lua_tostring(L, -1);
}

inline void LoadLuaArray(lua_State *L, const char *varName, std::set<std::string> &arr)
{
	lua_getglobal(L, varName);
	if (lua_istable(L, -1))
	{
		lua_pushnil(L);
		while (lua_next(L, -2) != 0)
		{
			if (lua_isstring(L, -1))
				arr.insert(lua_tostring(L, -1));
			lua_pop(L, 1);
		}
	}
}

inline void LoadLuaTable(lua_State *L, const char *varName, std::unordered_map<std::string, std::string> &table)
{
	lua_getglobal(L, varName);
	if (lua_istable(L, -1))
	{
		lua_pushnil(L);
		while (lua_next(L, -2) != 0)
		{
			if (lua_isstring(L, -1))
				table.insert({lua_tostring(L, -2), lua_tostring(L, -1)});
			lua_pop(L, 1);
		}
	}
}

int lua_SendToChat(lua_State* L)
{
	if (lua_gettop(L) == 2)
	{
		TwitchBot* tb = static_cast<TwitchBot*>(lua_touserdata(L, 1));
		if (tb->IsConnected())
			tb->Send("PRIVMSG #" + tb->GetChannelName() + " :" + lua_tostring(L, 2) + "\r\n");
	}
	else
		std::cerr << "[Global]:[lua_SendToChat] invalid number of args returned from lua.\n";
	return 0;
}

// TwitchBot class definitions
TwitchBot::TwitchBot()
{
	LoadConfig();
	Connect("irc.chat.twitch.tv", "6667");
	std::this_thread::sleep_for(std::chrono::milliseconds(500));
	LogIn();
}

TwitchBot::~TwitchBot()
{
	Disconnect();
}

void TwitchBot::Run()
{
	auto &messages = MessagesQueue();

	while (m_IsRunning)
	{
		if (messages.size() != 0)
		{
			ProcessMessage(messages.pop_back());
			std::this_thread::sleep_for(std::chrono::milliseconds(250));
		}
	}
}

void TwitchBot::LoadConfig()
{
	lua_State *L = luaL_newstate();
	luaL_openlibs(L);

	if (CheckLua(L, luaL_dofile(L, "config.lua")))
	{
		LoadLuaString(L, "_oauth", m_Oauth);
		LoadLuaString(L, "_botname", m_Botname);
		LoadLuaString(L, "_channel", m_Channel);
		LoadLuaArray(L, "_users", m_Users);
		LoadLuaArray(L, "_script_commands", m_ScriptCommands);
		LoadLuaTable(L, "_text_commands", m_TextCommands);
		LoadLuaTable(L, "_speech_commands", m_SpeechCommands);
		LoadLuaTable(L, "_emote_commands", m_EmoteCommands);
	}
	lua_close(L);
}

void TwitchBot::LogIn()
{
	if (IsConnected())
	{
		Send("PASS oauth:" + m_Oauth + "\r\n");
		Send("NICK " + m_Botname + "\r\n");
		Send("JOIN #" + m_Channel + "\r\n");
	}
}

void TwitchBot::AddCommand(const CommandType &type, const std::string &msg)
{
	switch (type)
	{
	case CommandType::TEXT:
		if (size_t pos = msg.find(" ", 5); pos != std::string::npos)
			m_TextCommands.insert({msg.substr(5, pos - 5), msg.substr(pos + 1, msg.size() - pos)});
		break;
	case CommandType::VOICE:  // TODO
	case CommandType::CUSTOM: // TODO
		break;
	}
}

void TwitchBot::RemoveCommand(const CommandType &type, const std::string &msg)
{
	switch (type)
	{
	case CommandType::TEXT:
		m_TextCommands.erase(msg.substr(4, msg.size() - 1));
		break;
	case CommandType::VOICE:  // TODO
	case CommandType::CUSTOM: // TODO
		break;
	}
}

void TwitchBot::ProcessMessage(const std::pair<std::string, std::string> &msg)
{
	if (msg.first != "")
		std::cout << msg.first << "\t\t--> " << msg.second << std::endl;
	if (msg.first == "nightbot")
		ScriptCommand(msg.first, msg.first);

	if (auto usr = m_Users.find(msg.first); usr != m_Users.end() || m_Users.find("all") != m_Users.end())
	{
		// Checking core !add, !rm and !play commands
		if ((msg.second).rfind("!add ", 0) == 0)
		{
			AddCommand(CommandType::TEXT, msg.second);
			return;
		}
		if ((msg.second).rfind("!rm ", 0) == 0)
		{
			RemoveCommand(CommandType::TEXT, msg.second);
			return;
		}
		if ((msg.second).rfind("!play ", 0) == 0)
		{
			SpeechCommand(msg.second.substr(6, msg.second.size()));
			return;
		}
		// Checking if command is defined
		if (auto it = m_TextCommands.find(msg.second); it != m_TextCommands.end())
			TextCommand(it->second);
		if (auto it = m_SpeechCommands.find(msg.second); it != m_SpeechCommands.end())
			SpeechCommand(it->second);
		if (auto it = m_EmoteCommands.find(msg.second); it != m_EmoteCommands.end())
			EmoteCommand(msg.first, it->second);
		if (auto it = m_ScriptCommands.find(msg.second); it != m_ScriptCommands.end())
			ScriptCommand(it->substr(1, it->size()), msg.first);
	}
}

void TwitchBot::TextCommand(std::string_view msg)
{
	if (IsConnected())
		Send("PRIVMSG #" + m_Channel + " :" + msg.data() + "\r\n");
}

void TwitchBot::SpeechCommand(std::string_view msg)
{
	TextToSpeech(msg.data());
}

void TwitchBot::EmoteCommand(std::string_view user, std::string_view msg)
{
	if (IsConnected())
		Send("PRIVMSG #" + m_Channel + " :" + user.data() + " " + msg.data() + "\r\n");
}

void TwitchBot::ScriptCommand(std::string_view cmd, std::string_view user)
{
	static lua_State *L;
	static bool bScriptLoaded = false;
	if (!bScriptLoaded)
	{
		L = luaL_newstate();
		luaL_openlibs(L);
		bScriptLoaded = CheckLua(L, luaL_dofile(L, "config.lua"));
		lua_register(L, "SendToChat", lua_SendToChat);
	}

	lua_getglobal(L, cmd.data());
	if (lua_isfunction(L, -1))
	{
		lua_pushlightuserdata(L, this);
		lua_pushstring(L, user.data());
		CheckLua(L, lua_pcall(L, 2, 1, 0));
	}
}

int main()
{
	std::cout << "\nHello streamer! I'm your new bot.\n" << std::endl;

	TwitchBot jaynebot;
	jaynebot.Run();

	return 0;
}