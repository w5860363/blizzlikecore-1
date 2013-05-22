/*
 * BlizzLikeCore Copyright (C) 2013  BlizzLikeGroup
 * Integrated Files: CREDITS.md and LICENSE.md
 */

#include <ace/OS_NS_signal.h>

#include "WorldSocketMgr.h"
#include "Common.h"
#include "Master.h"
#include "WorldSocket.h"
#include "WorldRunnable.h"
#include "World.h"
#include "Log.h"
#include "Timer.h"
#include "Policies/SingletonImp.h"
#include "SystemConfig.h"
#include "Config/Config.h"
#include "Database/DatabaseEnv.h"
#include "DBCStores.h"
#include "CliRunnable.h"
#include "RARunnable.h"
#include "Util.h"
#include "BCSoap.h"

// Format is BDB_YYYYMMDD
#ifndef _REQ_BDB_VERSION
# define _REQ_BDB_VERSION  "BDB_20130522"
#endif //_REQ_BDB_VERSION

#ifdef _WIN32
#include "ServiceWin32.h"
extern int m_ServiceStatus;
#endif

#ifdef _WIN32
# include <windows.h>
# define sleep(x) Sleep(x * 1000)
#else
# include <unistd.h>
#endif

INSTANTIATE_SINGLETON_1(Master);

volatile uint32 Master::m_masterLoopCounter = 0;

class FreezeDetectorRunnable : public ACE_Based::Runnable
{
public:
    FreezeDetectorRunnable() { _delaytime = 0; }
    uint32 m_loops, m_lastchange;
    uint32 w_loops, w_lastchange;
    uint32 _delaytime;
    void SetDelayTime(uint32 t) { _delaytime = t; }
    void run(void)
    {
        if (!_delaytime)
            return;
        sLog.outString("Starting up anti-freeze thread (%u seconds max stuck time)...",_delaytime/1000);
        m_loops = 0;
        w_loops = 0;
        m_lastchange = 0;
        w_lastchange = 0;
        while (!World::IsStopped())
        {
            ACE_Based::Thread::Sleep(1000);
            uint32 curtime = getMSTime();
            // normal work
            if (w_loops != World::m_worldLoopCounter)
            {
                w_lastchange = curtime;
                w_loops = World::m_worldLoopCounter;
            }
            // possible freeze
            else if (getMSTimeDiff(w_lastchange,curtime) > _delaytime)
            {
                sLog.outError("World Thread is stuck.  Terminating server!");
                *((uint32 volatile*)NULL) = 0;                       // bang crash
            }
        }
        sLog.outString("Anti-freeze thread exiting without problems.");
    }
};

Master::Master()
{
}

Master::~Master()
{
}

// Main function
int Master::Run()
{
    sLog.outString("**************************************************************************");
    sLog.outString(" %s(world) Rev: %s Hash: %s ", _PACKAGENAME, _REVISION, _HASH);
    sLog.outString("**************************************************************************");
    sLog.outString("<Ctrl-C> to stop.");
    sLog.outString(" ");

    sLog.outDetail("Using ACE: %s", ACE_VERSION);

#ifdef USE_SFMT_FOR_RNG
    sLog.outString("SFMT has been enabled as the random number generator, if problems occur");
    sLog.outString("first try disabling SFMT in CMAKE configuration..");
    sLog.outString(" ");
#endif //USE_SFMT_FOR_RNG

    // worldd PID file creation
    std::string pidfile = sConfig.GetStringDefault("PidFile", "");
    if (!pidfile.empty())
    {
        uint32 pid = CreatePIDFile(pidfile);
        if (!pid)
        {
            sLog.outError("Cannot create PID file %s.\n", pidfile.c_str());
            return 1;
        }

        sLog.outString("Daemon PID: %u\n", pid);
    }

    // Start the databases
    if (!_StartDB())
        return 1;

    // Initialize the World
    sWorld.SetInitialWorldSettings();

    // Catch termination signals
    _HookSignals();

    // Launch WorldRunnable thread
    ACE_Based::Thread world_thread(new WorldRunnable);
    world_thread.setPriority(ACE_Based::Highest);

    // set realmbuilds depend on worldserver expected builds, and set server online
    std::string builds = AcceptableClientBuildsListStr();
    LoginDatabase.escape_string(builds);
    LoginDatabase.PExecute("UPDATE realmlist SET realmflags = realmflags & ~(%u), population = 0, realmbuilds = '%s'  WHERE id = '%d'", REALM_FLAG_OFFLINE, builds.c_str(), realmID);

    ACE_Based::Thread* cliThread = NULL;

#ifdef _WIN32
    if (sConfig.GetBoolDefault("Console.Enable", true) && (m_ServiceStatus == -1)/* need disable console in service mode*/)
#else
    if (sConfig.GetBoolDefault("Console.Enable", true))
#endif
    {
        // Launch CliRunnable thread
        cliThread = new ACE_Based::Thread(new CliRunnable);
    }

    ACE_Based::Thread rar_thread(new RARunnable);

    // Handle affinity for multiple processors and process priority on Windows
    #ifdef _WIN32
    {
        HANDLE hProcess = GetCurrentProcess();

        uint32 Aff = sConfig.GetIntDefault("UseProcessors", 0);
        if (Aff > 0)
        {
            ULONG_PTR appAff;
            ULONG_PTR sysAff;

            if (GetProcessAffinityMask(hProcess,&appAff,&sysAff))
            {
                ULONG_PTR curAff = Aff & appAff;            // remove non accessible processors

                if (!curAff)
                {
                    sLog.outError("Processors marked in UseProcessors bitmask (hex) %x not accessible for worldserver. Accessible processors bitmask (hex): %x",Aff,appAff);
                }
                else
                {
                    if (SetProcessAffinityMask(hProcess,curAff))
                        sLog.outString("Using processors (bitmask, hex): %x", curAff);
                    else
                        sLog.outError("Can't set used processors (hex): %x",curAff);
                }
            }
            sLog.outString();
        }

        bool Prio = sConfig.GetBoolDefault("ProcessPriority", false);

        if (Prio)
        {
            if (SetPriorityClass(hProcess,HIGH_PRIORITY_CLASS))
                sLog.outString("WorldServer process priority class set to HIGH");
            else
                sLog.outError("ERROR: Can't set WorldServer process priority class.");
            sLog.outString();
        }
    }
    #endif

    // Start soap serving thread
    ACE_Based::Thread* soap_thread = NULL;

    if (sConfig.GetBoolDefault("SOAP.Enabled", false))
    {
        BCSoapRunnable *runnable = new BCSoapRunnable();

        runnable->setListenArguments(sConfig.GetStringDefault("SOAP.IP", "127.0.0.1"), sConfig.GetIntDefault("SOAP.Port", 7878));
        soap_thread = new ACE_Based::Thread(runnable);
    }

    uint32 realCurrTime, realPrevTime;
    realCurrTime = realPrevTime = getMSTime();

    //uint32 socketSelecttime = sWorld.getConfig(CONFIG_SOCKET_SELECTTIME);

    // Start up freeze catcher thread
    ACE_Based::Thread* freeze_thread = NULL;
    if (uint32 freeze_delay = sConfig.GetIntDefault("MaxCoreStuckTime", 0))
    {
        FreezeDetectorRunnable *fdr = new FreezeDetectorRunnable();
        fdr->SetDelayTime(freeze_delay*1000);
        freeze_thread = new ACE_Based::Thread(fdr);
        freeze_thread->setPriority(ACE_Based::Highest);
    }

    // Launch the world listener socket
    uint16 wsport = sWorld.getConfig(CONFIG_PORT_WORLD);
    std::string bind_ip = sConfig.GetStringDefault ("BindIP", "0.0.0.0");

    if (sWorldSocketMgr->StartNetwork (wsport, bind_ip.c_str ()) == -1)
    {
        sLog.outError("Failed to start network");
        World::StopNow(ERROR_EXIT_CODE);
        // go down and shutdown the server
    }

    sWorldSocketMgr->Wait();

    // Stop freeze protection before shutdown tasks
    if (freeze_thread)
    {
        freeze_thread->destroy();
        delete freeze_thread;
    }

    // Stop soap thread
    if (soap_thread)
    {
        soap_thread->wait();
        soap_thread->destroy();
        delete soap_thread;
    }

    // Set server offline in realmlist
    LoginDatabase.PExecute("UPDATE realmlist SET realmflags = realmflags | %u WHERE id = '%d'", REALM_FLAG_OFFLINE, realmID);

    // Remove signal handling before leaving
    _UnhookSignals();

    // when the main thread closes the singletons get unloaded
    // since worldrunnable uses them, it will crash if unloaded after master
    world_thread.wait();
    rar_thread.wait ();

    // Clean account database before leaving
    clearOnlineAccounts();

    // Wait for delay threads to end
    CharacterDatabase.HaltDelayThread();
    WorldDatabase.HaltDelayThread();
    LoginDatabase.HaltDelayThread();

    sLog.outString("Halting process...");

    if (cliThread)
    {
        #ifdef _WIN32

        // this only way to terminate CLI thread exist at Win32 (alt. way exist only in Windows Vista API)
        //_exit(1);
        // send keyboard input to safely unblock the CLI thread
        INPUT_RECORD b[5];
        HANDLE hStdIn = GetStdHandle(STD_INPUT_HANDLE);
        b[0].EventType = KEY_EVENT;
        b[0].Event.KeyEvent.bKeyDown = TRUE;
        b[0].Event.KeyEvent.uChar.AsciiChar = 'X';
        b[0].Event.KeyEvent.wVirtualKeyCode = 'X';
        b[0].Event.KeyEvent.wRepeatCount = 1;

        b[1].EventType = KEY_EVENT;
        b[1].Event.KeyEvent.bKeyDown = FALSE;
        b[1].Event.KeyEvent.uChar.AsciiChar = 'X';
        b[1].Event.KeyEvent.wVirtualKeyCode = 'X';
        b[1].Event.KeyEvent.wRepeatCount = 1;

        b[2].EventType = KEY_EVENT;
        b[2].Event.KeyEvent.bKeyDown = TRUE;
        b[2].Event.KeyEvent.dwControlKeyState = 0;
        b[2].Event.KeyEvent.uChar.AsciiChar = '\r';
        b[2].Event.KeyEvent.wVirtualKeyCode = VK_RETURN;
        b[2].Event.KeyEvent.wRepeatCount = 1;
        b[2].Event.KeyEvent.wVirtualScanCode = 0x1c;

        b[3].EventType = KEY_EVENT;
        b[3].Event.KeyEvent.bKeyDown = FALSE;
        b[3].Event.KeyEvent.dwControlKeyState = 0;
        b[3].Event.KeyEvent.uChar.AsciiChar = '\r';
        b[3].Event.KeyEvent.wVirtualKeyCode = VK_RETURN;
        b[3].Event.KeyEvent.wVirtualScanCode = 0x1c;
        b[3].Event.KeyEvent.wRepeatCount = 1;
        DWORD numb;
        WriteConsoleInput(hStdIn, b, 4, &numb);

        cliThread->wait();

        #else

        cliThread->destroy();

        #endif

        delete cliThread;
    }

    // for some unknown reason, unloading scripts here and not in worldrunnable
    // fixes a memory leak related to detaching threads from the module
    //UnloadScriptingModule();

    // Exit the process with specified return value
    return World::GetExitCode();
}

// Initialize connection to the databases
bool Master::_StartDB()
{
    sLog.SetLogDB(false);

    // Get world database info from configuration file
    std::string dbstring = sConfig.GetStringDefault("WorldDatabaseInfo", "");
    if (dbstring.empty())
    {
        sLog.outError("World database not specified in configuration file");
        sleep(5);
        return false;
    }

    // Initialise the world database
    if (!WorldDatabase.Initialize(dbstring.c_str()))
    {
        sLog.outError("BC> Can't connect to database at %s", dbstring.c_str());
        sleep(5);
        return false;
    }

    // Get character database info from configuration file
    dbstring = sConfig.GetStringDefault("CharacterDatabaseInfo", "");
    if (dbstring.empty())
    {
        sLog.outError("Character database not specified in configuration file");
        sleep(5);
        return false;
    }

    // Initialise the Character database
    if (!CharacterDatabase.Initialize(dbstring.c_str()))
    {
        sLog.outError("Cannot connect to Character database %s",dbstring.c_str());
        sleep(5);
        return false;
    }

    // Get login database info from configuration file
    dbstring = sConfig.GetStringDefault("LoginDatabaseInfo", "");
    if (dbstring.empty())
    {
        sLog.outError("Login database not specified in configuration file");
        sleep(5);
        return false;
    }

    // Initialise the login database
    if (!LoginDatabase.Initialize(dbstring.c_str()))
    {
        sLog.outError("Cannot connect to login database %s",dbstring.c_str());
        sleep(5);
        return false;
    }

    // Get the realm Id from the configuration file
    realmID = sConfig.GetIntDefault("RealmID", 0);
    if (!realmID)
    {
        sLog.outError("Realm ID not defined in configuration file");
        sleep(5);
        return false;
    }
    sLog.outString("Realm running as realm ID %d", realmID);

    // Initialize the DB logging system
    sLog.SetLogDBLater(sConfig.GetBoolDefault("EnableLogDB", false)); // set var to enable DB logging once startup finished.
    sLog.SetLogDB(false);
    sLog.SetRealmID(realmID);

    // Clean the database before starting
    clearOnlineAccounts();

    // Insert version info into DB
    WorldDatabase.PExecute("UPDATE version SET core_version = '%s', core_revision = '%s'", _FULLVERSION, _REVISION);

    // Check DB version
    sWorld.LoadDBVersion();

    const char* db_version = sWorld.GetDBVersion();
	if (strcmp(db_version, _REQ_BDB_VERSION) != 0)
    {
        sLog.outError(" WARNING:");
        sLog.outError(" Your World DB version: %s is wrong.", db_version);
        sLog.outError(" Required World DB version: %s", _REQ_BDB_VERSION);
        sleep(5);
        return false;
    }

    sLog.outString("Using World DB: %s", db_version);
    return true;
}

// Clear 'online' status for all accounts with characters in this realm
void Master::clearOnlineAccounts()
{
    // Cleanup online status for characters hosted at current realm
    // todo - Only accounts with characters logged on *this* realm should have online status reset. Move the online column from 'account' to 'realmcharacters'?
    LoginDatabase.PExecute("UPDATE account SET active_realm_id = 0 WHERE active_realm_id = '%d'", realmID);

    CharacterDatabase.Execute("UPDATE characters SET online = 0 WHERE online<>0");

    // Battleground instance ids reset at server restart
//  CharacterDatabase.Execute("UPDATE character_battleground_data SET instance_id = 0");
}

// Handle termination signals
void Master::_OnSignal(int s)
{
    switch (s)
    {
        case SIGINT:
            World::StopNow(RESTART_EXIT_CODE);
            break;
        case SIGTERM:
        #ifdef _WIN32
        case SIGBREAK:
        #endif
            World::StopNow(SHUTDOWN_EXIT_CODE);
            break;
    }

    signal(s, _OnSignal);
}

// Define hook '_OnSignal' for all termination signals
void Master::_HookSignals()
{
    signal(SIGINT, _OnSignal);
    signal(SIGTERM, _OnSignal);
    #ifdef _WIN32
    signal(SIGBREAK, _OnSignal);
    #endif
}

// Unhook the signals before leaving
void Master::_UnhookSignals()
{
    signal(SIGINT, 0);
    signal(SIGTERM, 0);
    #ifdef _WIN32
    signal(SIGBREAK, 0);
    #endif
}

