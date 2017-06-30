#include "plugin.h"

#define dprintf(str, ...) _plugin_logprintf("[" PLUGIN_NAME "] " str, __VA_ARGS__)

static std::unordered_map<std::string, std::vector<DBGPATCHINFO>> modPatches;
static std::unordered_map<std::string, duint> modBases;
static std::vector<DBGPATCHINFO> patches;

static void applyModPatches(duint base, std::string modname)
{
    //Apply the patches to the module.
    auto found = modPatches.find(modname);
    if(found != modPatches.end())
    {
        auto applied = 0;
        for(auto & patch : found->second)
        {
            auto va = base + patch.addr;
            if(Script::Memory::ReadByte(va) == patch.oldbyte) //check if the bytes match
                if(DbgFunctions()->MemPatch(va, &patch.newbyte, 1)) //check if the patch is applied correctly
                    applied++;
        }
        dprintf("Applied %d/%d patches\n", applied, int(found->second.size()));
    }
    //Store the newest modbase to adjust the patches (DBGPATCHINFO.addr is a VA and on exit the module bases are unknown).
    //Likely this should be fixed in x64dbg instead of this hacky workaround.
    modBases[modname] = base;
}

PLUG_EXPORT void CBLOADDLL(CBTYPE, PLUG_CB_LOADDLL* info)
{
    applyModPatches(duint(info->modInfo->BaseOfImage), info->modname);
}

PLUG_EXPORT void CBCREATEPROCESS(CBTYPE, PLUG_CB_CREATEPROCESS* info)
{
    auto base = duint(info->modInfo->BaseOfImage);
    char modname[MAX_MODULE_SIZE] = "";
    if(DbgFunctions()->ModNameFromAddr(base, modname, true)) //retrieve the module name compatible with the database
        applyModPatches(base, modname);
}

PLUG_EXPORT void CBLOADDB(CBTYPE, PLUG_CB_LOADSAVEDB* info)
{
    auto jpatches = json_object_get(info->root, PLUGIN_NAME);
    size_t i;
    json_t* jpatch;
    patches.clear();
    json_array_foreach(jpatches, i, jpatch) //https://jansson.readthedocs.io/en/2.10/apiref.html#c.json_array_foreach
    {
        DBGPATCHINFO patch;
        strncpy_s(patch.mod, json_string_value(json_object_get(jpatch, "mod")), _TRUNCATE);
        patch.addr = (duint)json_integer_value(json_object_get(jpatch, "addr"));
        patch.oldbyte = (unsigned char)json_integer_value(json_object_get(jpatch, "oldbyte"));
        patch.newbyte = (unsigned char)json_integer_value(json_object_get(jpatch, "newbyte"));
        patches.push_back(patch);
    }
    for(auto & patch : patches)
        modPatches[patch.mod].push_back(patch);
    dprintf("loaded %d patches from database\n", int(patches.size()));
}

PLUG_EXPORT void CBEXITPROCESS(CBTYPE, PLUG_CB_EXITPROCESS* info)
{
    //Save the patches on process exit (PatchEnum fails if not debugging, so we save them on process exit and use them later if PatchEnum fails).
    size_t cbsize;
    if(DbgFunctions()->PatchEnum(nullptr, &cbsize))
    {
        patches.resize(cbsize / sizeof(DBGPATCHINFO));
        if(!DbgFunctions()->PatchEnum(patches.data(), nullptr))
            patches.clear();
    }
    else
        dprintf("PatchEnum failed (1)\n");
}

static void savePatches(json_t* root)
{
    auto jpatches = json_array();
    for(const auto & patch : patches)
    {
        if(!*patch.mod)
            continue; //skip non-module patches
        auto found = modBases.find(patch.mod);
        if(found == modBases.end())
            continue; //skip patches without known module base
        auto jpatch = json_object();
        json_object_set_new(jpatch, "mod", json_string(patch.mod));
        json_object_set_new(jpatch, "addr", json_integer(patch.addr - found->second));
        json_object_set_new(jpatch, "oldbyte", json_integer(patch.oldbyte));
        json_object_set_new(jpatch, "newbyte", json_integer(patch.newbyte));
        json_array_append_new(jpatches, jpatch);
    }
    json_object_set_new(root, PLUGIN_NAME, jpatches);
    dprintf("saved %d patches to database\n", int(patches.size()));
}

PLUG_EXPORT void CBSAVEDB(CBTYPE, PLUG_CB_LOADSAVEDB* info)
{
    //This functions can be called in two ways:
    //1. the "dbsave" command
    //2. after the debuggee stopped
    size_t cbsize;
    if(DbgFunctions()->PatchEnum(nullptr, &cbsize)) //this only succeeds on the "dbsave" command
    {
        patches.resize(cbsize / sizeof(DBGPATCHINFO));
        if(DbgFunctions()->PatchEnum(patches.data(), nullptr))
            savePatches(info->root);
        else
            dprintf("PatchEnum failed (2)\n");
    }
    else //PatchEnum failed -> we use the patches from CBEXITPROCESS
        savePatches(info->root);
}

//Initialize your plugin data here.
bool pluginInit(PLUG_INITSTRUCT* initStruct)
{
    return true; //Return false to cancel loading the plugin.
}

//Deinitialize your plugin data here (clearing menus optional).
bool pluginStop()
{
    return true;
}

//Do GUI/Menu related things here.
void pluginSetup()
{
}
