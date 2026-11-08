// Game-side entry: mirrors upstream pc_main.cpp after window/settings init. No <3ds.h> here.
#include "n3ds_bridge.h"
#include "system.h"
#include "App.h"
#include "sysNew.h"

extern "C" void n3ds_game_entry(void)
{
    n3ds_log("[n3ds] gsys->Initialise()");
    gsys->Initialise();
    n3ds_log_memory("after Initialise");

    n3ds_log("[n3ds] creating NodeMgr");
    nodeMgr = new NodeMgr();

    n3ds_log("[n3ds] gsys->run(PlugPikiApp)");
    gsys->run(new PlugPikiApp());
}
