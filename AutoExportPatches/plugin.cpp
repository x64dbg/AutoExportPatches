#include "plugin.h"
#include <fstream>
#include <string>

#define dprintf(str, ...) _plugin_logprintf("[" PLUGIN_NAME "] " str, __VA_ARGS__)

std::string config_path;
static std::unordered_map<std::string, std::vector<DBGPATCHINFO>> modPatches;
static std::unordered_map<std::string, duint> modBases;
static std::vector<DBGPATCHINFO> patches;
static bool enablePatches;

// Function to read the plugin configuration
void ReadConfig()
{
    std::ifstream infile(config_path);
    std::string line;
    if (infile.is_open())
    {
        while (std::getline(infile, line))
        {
            if (line.find("auto_export_patches=") != std::string::npos)
            {
                enablePatches = (line.substr(line.find("=") + 1) == "true");
                break;
            }
        }
        infile.close();
    }
    else
    {
        enablePatches = true; // Default value if config file not found
    }
}

// Function to write the plugin configuration
void WriteConfig()
{
    std::ofstream outfile(config_path);
    if (outfile.is_open())
    {
        outfile << "[settings]\n";
        outfile << "auto_export_patches=" << (enablePatches ? "true" : "false") << "\n";
        outfile.close();
    }
}

// Function to apply patches to a module
static void applyModPatches(duint base, std::string modname)
{
    // Apply the patches to the module only if enabled
    if (!enablePatches)
        return;

    auto found = modPatches.find(modname);
    if (found != modPatches.end())
    {
        auto applied = 0;
        for (auto& patch : found->second)
        {
            auto va = base + patch.addr;
            if (Script::Memory::ReadByte(va) == patch.oldbyte) // Check if the bytes match
                if (DbgFunctions()->MemPatch(va, &patch.newbyte, 1)) // Check if the patch is applied correctly
                    applied++;
        }
        dprintf("Applied %d/%d patches\n", applied, int(found->second.size()));
    }
    // Store the newest modbase to adjust the patches (DBGPATCHINFO.addr is a VA, and on exit, the module bases are unknown).
    // Likely this should be fixed in x64dbg instead of this hacky workaround.
    modBases[modname] = base;
}

// Callback function for DLL load events
PLUG_EXPORT void CBLOADDLL(CBTYPE, PLUG_CB_LOADDLL* info)
{
    applyModPatches(duint(info->modInfo->BaseOfImage), info->modname);
}

// Callback function for process creation events
PLUG_EXPORT void CBCREATEPROCESS(CBTYPE, PLUG_CB_CREATEPROCESS* info)
{
    auto base = duint(info->modInfo->BaseOfImage);
    char modname[MAX_MODULE_SIZE] = "";
    if (DbgFunctions()->ModNameFromAddr(base, modname, true)) // Retrieve the module name compatible with the database
        applyModPatches(base, modname);
}

// Callback function for loading saved database events
PLUG_EXPORT void CBLOADDB(CBTYPE, PLUG_CB_LOADSAVEDB* info)
{
    auto jpatches = json_object_get(info->root, PLUGIN_NAME);
    size_t i;
    json_t* jpatch;
    patches.clear();
    modPatches.clear();
    json_array_foreach(jpatches, i, jpatch) // https://jansson.readthedocs.io/en/2.10/apiref.html#c.json_array_foreach
    {
        DBGPATCHINFO patch;
        strncpy_s(patch.mod, json_string_value(json_object_get(jpatch, "mod")), _TRUNCATE);
        patch.addr = (duint)json_integer_value(json_object_get(jpatch, "addr"));
        patch.oldbyte = (unsigned char)json_integer_value(json_object_get(jpatch, "oldbyte"));
        patch.newbyte = (unsigned char)json_integer_value(json_object_get(jpatch, "newbyte"));
        patches.push_back(patch);
    }
    for (auto& patch : patches)
        modPatches[patch.mod].push_back(patch);
    dprintf("loaded %d patches from the database\n", int(patches.size()));
}

// Callback function for process exit events
PLUG_EXPORT void CBEXITPROCESS(CBTYPE, PLUG_CB_EXITPROCESS* info)
{
    // Save the patches on process exit (PatchEnum fails if not debugging, so we save them on process exit and use them later if PatchEnum fails).
    size_t cbsize;
    if (DbgFunctions()->PatchEnum(nullptr, &cbsize))
    {
        patches.resize(cbsize / sizeof(DBGPATCHINFO));
        if (!DbgFunctions()->PatchEnum(patches.data(), nullptr))
            patches.clear();
    }
    else
    {
        dprintf("PatchEnum failed (1)\n");
    }
}

// Function to save patches to the database
static void savePatches(json_t* root)
{
    auto jpatches = json_array();
    for (const auto& patch : patches)
    {
        if (!*patch.mod)
            continue; // Skip non-module patches
        auto found = modBases.find(patch.mod);
        if (found == modBases.end())
            continue; // Skip patches without a known module base
        auto jpatch = json_object();
        json_object_set_new(jpatch, "mod", json_string(patch.mod));
        json_object_set_new(jpatch, "addr", json_integer(patch.addr - found->second));
        json_object_set_new(jpatch, "oldbyte", json_integer(patch.oldbyte));
        json_object_set_new(jpatch, "newbyte", json_integer(patch.newbyte));
        json_array_append_new(jpatches, jpatch);
    }
    json_object_set_new(root, PLUGIN_NAME, jpatches);
    dprintf("saved %d patches to the database\n", int(patches.size()));
}

// Callback function for saving the database
PLUG_EXPORT void CBSAVEDB(CBTYPE, PLUG_CB_LOADSAVEDB* info)
{
    // This function can be called in two ways:
    // 1. the "dbsave" command
    // 2. after the debuggee stopped
    size_t cbsize;
    if (DbgFunctions()->PatchEnum(nullptr, &cbsize)) // This only succeeds on the "dbsave" command
    {
        patches.resize(cbsize / sizeof(DBGPATCHINFO));
        if (DbgFunctions()->PatchEnum(patches.data(), nullptr))
            savePatches(info->root);
        else
            dprintf("PatchEnum failed (2)\n");
    }
    else // PatchEnum failed -> we use the patches from CBEXITPROCESS
    {
        savePatches(info->root);
    }
}

// Initialize your plugin data here
bool pluginInit(PLUG_INITSTRUCT* initStruct)
{
    char szCurrentDirectory[MAX_PATH];
    GetCurrentDirectory(MAX_PATH, szCurrentDirectory);
    config_path = std::string(szCurrentDirectory) + "\\AutoExportPatches.ini";
    ReadConfig();
    return true;
}

bool pluginStop()
{
    WriteConfig();
    return true;
}

void pluginSetup()
{
    std::string submenuText = enablePatches ? "Auto Export Patches: On" : "Auto Export Patches: Off";
    _plugin_menuaddentry(hMenu, 1, submenuText.c_str());
}

// Menu entry callback function
PLUG_EXPORT void CBMENUENTRY(CBTYPE cbType, PLUG_CB_MENUENTRY* info)
{
    if (info->hEntry == 1)
    {
        enablePatches = !enablePatches;
        WriteConfig(); // Write the new setting to the config file
        dprintf("Auto Export Patches is %s\n", enablePatches ? "On" : "Off");

        // Delete the old menu item and add the new one with updated text
        _plugin_menuclear(hMenu);
        std::string submenuText = enablePatches ? "Auto Export Patches: On" : "Auto Export Patches: Off";
        _plugin_menuaddentry(hMenu, 1, submenuText.c_str());
    }
}
