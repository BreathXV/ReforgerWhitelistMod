class ReforgerWhitelist
{
	// SCR_JsonLoadContext context = new SCR_JsonLoadContext();
	static ref ServerAdminTools_ConfigStruct m_config;
	static string m_configFilePath = "$profile:ReforgerWhitelist_Config.json";

	static ref array< ref ServerAdminTools_RepeatedChatMessage > m_repeatedChatMessages = {};
	static ref array< ref ServerAdminTools_ScheduledChatMessage > m_scheduledChatMessages = {};
	static ref map< int, ref ServerAdminTools_CycleMessageInterval > m_chatMessageCycleIntervals = new map< int, ref ServerAdminTools_CycleMessageInterval >();

	static ref ServerAdminTools_Stats m_stats;
	
	static ref ServerAdminTools_RestCallback m_restCallbackEvents = new ServerAdminTools_RestCallback();
	
	// parsed server message
	static string serverMessage = "";
	
	// keeps track of players 
	static ref map<string, ref ServerAdminTools_PlayerEntryStruct> m_playerHistory = new map<string, ref ServerAdminTools_PlayerEntryStruct>();
	static ref ServerAdminTools_EventDataBuffer m_eventDataBuffer;
	static bool m_isRateLimited = false;
	static int m_eventsNextMessageAt = 0;
		
	static string m_hexTable = "0123456789ABCDEF";
	static string IntToHex(int number)
	{
		number = Math.AbsInt(number);
		
		if (number < 16)
			return m_hexTable[number];
		
		string output = "";
		int remainder = 0;
		
		while (number > 0) {
			remainder = number % 16;
			output = m_hexTable[remainder] + output;
			number = Math.Floor(number / 16);
		};
		
		return output;
	};
	
	static void KeyReadError(string key)
	{
		Print("ServerAdminTools.KeyReadError | Configuration key "+key+" is missing or invalid. Will replace with defaults.", LogLevel.WARNING);
		m_config.SetDefaults(key);
	};
	
	static bool LoadConfig()
	{
		Print("ServerAdminTools.LoadConfig | Trying to load configuration file "+m_configFilePath, LogLevel.NORMAL);
		
		SCR_JsonLoadContext configLoadContext = new SCR_JsonLoadContext();
		
		m_config = new ServerAdminTools_ConfigStruct();
				
		if (!FileIO.FileExists( m_configFilePath ))
		{
			Print("ServerAdminTools.LoadConfig | Configuration file does not exist. Will try to create a template.", LogLevel.WARNING);
			m_config.SetDefaultsAll();
			// SaveConfig();
		}
		else
		{
			if (!configLoadContext.LoadFromFile( m_configFilePath ))
			{
				Print("ServerAdminTools.LoadConfig | Configuration load failed. Please ensure it exists and is in correct format.", LogLevel.ERROR);
				return false;
			};
			
			if (!configLoadContext.ReadValue("", m_config))
			{
				Print("ServerAdminTools.LoadConfig | Configuration load failed. Please ensure it exists and is in correct format.", LogLevel.ERROR);
				return false;
			};
		};
		
		/*
		if (m_config.serverMessageHeaderImage != "mission" && m_config.serverMessageHeaderImage != "")
		{
			Resource imageCheck = Resource.Load(m_config.serverMessageHeaderImage);
			if (!imageCheck.IsValid())
				KeyReadError("serverMessageHeaderImage");
		};
		*/
		
		if (!m_config.eventsApiEventsEnabled)
			KeyReadError("eventsApiEventsEnabled");
		
		if (!m_config.repeatedChatMessages)
			KeyReadError("repeatedChatMessages");
		
		if (!m_config.scheduledChatMessages)
			KeyReadError("scheduledChatMessages");
		
		serverMessage = "";
		foreach (string part : m_config.serverMessage) {
			if (part.EndsWith(">")) {
				serverMessage = string.Format("%1%2", serverMessage, part);
				continue;
			};
			
			serverMessage = string.Format("%1%2\n", serverMessage, part);
		};
		
		// serverMessage = SCR_StringHelper.Join("\n", m_config.serverMessage, true);
		if (serverMessage.Length() > 2048) {
			Print("ServerAdminTools.LoadConfig | Server message is longer than 2048 characters. It may not work!", LogLevel.WARNING);
		};
		
		LoadChatMessages();
		
		if (m_config.repeatedChatMessagesCycle == true)
			SetupCycleLookupTable();
		
		m_stats = new ServerAdminTools_Stats();
		
		if (m_config.banReloadIntervalMinutes > 0)
			GetGame().GetCallqueue().CallLater(ServerAdminTools.ReloadBans, m_config.banReloadIntervalMinutes * 60 * 1000, true);
		
		m_eventDataBuffer = new ServerAdminTools_EventDataBuffer();
		m_eventDataBuffer.token = m_config.eventsApiToken;
		
		if (m_config.eventsApiRatelimitSeconds < 1) {
			Print("ServerAdminTools.LoadConfig | eventsApiRatelimitSeconds is <1, changing to default 10", LogLevel.WARNING);
			m_config.eventsApiRatelimitSeconds = 10;
		};
		
		SaveConfig();
		
		return true;
	};

	static bool ReloadBans()
	{
		SCR_JsonLoadContext configLoadContext = new SCR_JsonLoadContext();
		if (!configLoadContext.LoadFromFile( m_configFilePath ))
		{
			Print("ServerAdminTools.ReloadBans | Configuration load failed. Please ensure it exists and is in correct format.", LogLevel.ERROR);
			return false;
		};
		
		if (!configLoadContext.ReadValue("bans", m_config.bans))
		{
			Print("ServerAdminTools.ReloadBans | Reloading bans failed, something may be wrong with the config file.", LogLevel.ERROR);
			return false;
		};
		
		PlayersBannedCheck();
		
		return true; 
	};
	
	// -- return player Identity ID 
	static string GetPlayerUID(int playerId)
	{		
		BackendApi api = GetGame().GetBackendApi();
		
		if (!api)
		{
			Print("ServerAdminTools.GetPlayerUID | Failed to get backend API!", LogLevel.ERROR);
			return "";
		};
		
		string identityId = api.GetPlayerIdentityId(playerId);
		
		Print("ServerAdminTools.GetPlayerUID | Identity ID: "+identityId, LogLevel.DEBUG);
		
		return identityId;
	};
	
	static bool SaveConfig()
	{
		SCR_JsonSaveContext configSaveContext = new SCR_JsonSaveContext();
		configSaveContext.WriteValue("", m_config);
		
		if (!configSaveContext.SaveToFile( m_configFilePath ))
		{
			Print("ServerAdminTools.SaveConfig | Saving config file failed!", LogLevel.ERROR);
			return false;
		};
		
		return true;
	};
	
	// events
	static void PublishEvent(string eventName, string eventTitle, ServerAdminTools_EventData eventData) {		
		Print("ServerAdminTools | Event "+eventName+" | "+eventData.Repr());
		
		m_stats.OnEventFired(eventName);
		
		if (m_config.eventsApiToken == "" || m_config.eventsApiAddress == "")
			return;
		
		ServerAdminTools_EventDataBufferItem newEvent = new ServerAdminTools_EventDataBufferItem(eventName, eventTitle, eventData);
		m_eventDataBuffer.events.Insert(newEvent);
		
		// return if already rate limited
		if (m_isRateLimited) {
			Print("ServerAdminTools.PublishEvent | Events are currently rate limited.", LogLevel.DEBUG);
			return;
		};
		
		// rate limit check
		if (System.GetUnixTime() < m_eventsNextMessageAt) {
			// send events after some seconds
			m_isRateLimited = true;
				
			GetGame().GetCallqueue().CallLater(ServerAdminTools.SendEvents, m_config.eventsApiRatelimitSeconds*1000, false);
			
			Print("ServerAdminTools.PublishEvent | Rate limiting events.", LogLevel.DEBUG);
			return;
		};
		
		// can send event immediately
		SendEvents();
	};
	
	// send events and update last request time
	static void SendEvents() {
		m_eventsNextMessageAt = m_config.eventsApiRatelimitSeconds + System.GetUnixTime();
		
		// m_eventDataBuffer.Pack();
		
		SCR_JsonSaveContext jsonCtx = new SCR_JsonSaveContext();
		jsonCtx.WriteValue("", m_eventDataBuffer);
		
		RestContext ctx = GetGame().GetRestApi().GetContext(m_config.eventsApiAddress);

		ctx.POST(m_restCallbackEvents, "", jsonCtx.ExportToString());
		
		m_eventDataBuffer.ClearEvents();
		m_isRateLimited = false;
	};
	
	static void MakePostRequest() {
        SCR_JsonSaveContext jsonCtx = new SCR_JsonSaveContext();
        jsonCtx.WriteValue("hello", "there");
        
        RestContext ctx = GetGame().GetRestApi().GetContext("https://bacontest.requestcatcher.com/hello");

        ctx.POST(m_restCallbackEvents, "", jsonCtx.ExportToString());
    };
};

class ServerAdminTools_RestCallback: RestCallback {
	override void OnError( int errorCode )
	{
		Print("ServerAdminTools | Events API POST request error: "+errorCode, LogLevel.ERROR);
	};

	/**
	\brief Called in case request timed out or handled improperly (no error, no success, no data)
	*/
	override void OnTimeout()
	{
		Print("ServerAdminTools | Events API POST request timed out", LogLevel.ERROR);
	};

	/**
	\brief Called when data arrived and/ or response processed successfully
	*/
	override void OnSuccess( string data, int dataSize )
	{
		Print(data);
	};
}