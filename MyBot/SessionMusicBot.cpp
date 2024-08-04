﻿#include "SessionMusicBot.h"				//Pre-written sanity checks for versions
#include <dpp/dpp.h>						//D++ header
#include "fmod.hpp"							//FMOD Core
#include "fmod_studio.hpp"					//FMOD Studio
#include "fmod_common.h"
#include "fmod_studio_common.h"
#include "fmod_errors.h"					//Allows FMOD Results to be output as understandable text
#include <filesystem>						//Standard C++ Filesystem
#ifndef NDEBUG
#include <chrono>							//Standard C++ Timekeeping (mostly for debugging?)
#endif
#include "SessionMusicBot_Utils.h"			//Some utility functions specific to this bot

/* Be sure to place your token in the line below.
 * Follow steps here to get a token:
 * https://dpp.dev/creating-a-bot-application.html
 * When you invite the bot, be sure to invite it with the
 * scopes 'bot' and 'applications.commands'.
 */

//---File Paths---//
std::filesystem::path exe_path;
std::filesystem::path bank_dir_path;

//---FMOD Declarations---//
FMOD::Studio::System* pSystem = nullptr;				//overall system
FMOD::System* pCoreSystem = nullptr;					//overall core system
FMOD::Studio::Bus* pMasterBus = nullptr;				//Master bus
FMOD::ChannelGroup* pMasterBusGroup = nullptr;			//Channel Group of the master bus
FMOD::DSP* mCaptureDSP = nullptr;						//DSP to attach to Master Channel Group for stealing output

//---Banks and stuff---//
FMOD::Studio::Bank* pMasterBank = nullptr;							//Master Bank, always loads first and contains shared content
std::string master_bank = "Master.bank";
FMOD::Studio::Bank* pMasterStringsBank = nullptr;					//Master Strings, allows us to refer to events by name instead of GUID
std::string masterstrings_bank = "Master.strings.bank";
std::vector<FMOD::Studio::Bank*> pBanks;							// Vector of all other banks
std::vector<std::filesystem::path> bankPaths;						// Vector of paths to the respective .bank files (at time of load)

const std::string callableEventPath = "event:/Master/";				// The FMOD-internal path where our callable events exist
std::map<std::string, sessionEventDesc> pEventDescriptions;			// Map of all Event Descriptions and their parameters
std::vector<std::string> eventPaths;								// Vector of FMOD-internal paths the user can call
std::map<std::string, sessionEventInstance> pEventInstances;		// Map of all Event Instances, which pair user-given name to instance

// Busses
const std::string busPath = "bus:/";
std::vector<std::string> busPaths;				// Vector of FMOD-internal paths to each bus
std::map <std::string, FMOD::Studio::Bus*> pBusses;
const float DISCORD_MASTERBUS_VOLOFFSET = -10.0f;

// VCAs
const std::string vcaPath = "vca:/";
std::vector<std::string> vcaPaths;
std::map <std::string, FMOD::Studio::VCA*> pVCAs;

// Snapshots
const std::string snapshotPath = "snapshot:/";
std::vector<std::string> snapshotPaths;
std::map <std::string, FMOD::Studio::EventDescription*> pSnapshots;
std::map<std::string, FMOD::Studio::EventInstance*> pSnapshotInstances;

FMOD_3D_ATTRIBUTES listenerAttributes;								// Holds the listener's position & orientation (at the origin). Not yet used (todo: 5.1 or 4.0 mixdown DSP?)

//---Misc Bot Declarations---//
dpp::discord_voice_client* currentClient = nullptr;		// Current Voice Client of the bot. Only designed to run on one server.
std::vector<int16_t> myPCMData;							// Main buffer of PCM audio data, which FMOD adds to and D++ cuts "frames" from
bool exitRequested = false;								// Set to "true" when you want off Mr. Bones Wild Tunes.
bool isConnected = false;								// Set to "true" when bot is connected to a Voice Channel.
std::vector<dpp::snowflake> owningUsers;				// Vector of the bot's owner and whitelisted users (todo: whitelisting). Owner is always first value.
dpp::embed basicEmbed = dpp::embed()					// Generic embed with all the shared details
	.set_color(dpp::colors::construction_cone_orange)
	.set_timestamp(time(0));

#ifndef NDEBUG
//---Extra Variables only present in Debug mode, for extra data---//
int samplesAddedCounter = 0;
#endif

//---FMOD and Audio Functions---//
// Callback for stealing sample data from the Master Bus
FMOD_RESULT F_CALLBACK captureDSPReadCallback(FMOD_DSP_STATE* dsp_state, float* inbuffer, float* outbuffer, unsigned int length, int inchannels, int* outchannels) {

	FMOD::DSP* thisdsp = (FMOD::DSP*)dsp_state->instance;

	if (isConnected) {
		switch (inchannels) {
			case 1:																				//Mono Input
				if (*outchannels == 1) {
					for (unsigned int samp = 0; samp < length; samp++) {
						outbuffer[(samp * *outchannels)] = 0.0f;										//Brute force mutes system output
						myPCMData.push_back(floatToPCM(inbuffer[samp]));					//Adds sample to PCM buffer...
						myPCMData.push_back(floatToPCM(inbuffer[samp]));					//...twice, because D++ expects stereo input
#ifndef NDEBUG
						samplesAddedCounter += 2;
#endif
					}
				}
				else {
					std::cout << "Mono in, not mono out!" << std::endl;
				}
				break;

			case 2:																				//Stereo Input
				for (unsigned int samp = 0; samp < length; samp++) {
					for (int chan = 0; chan < *outchannels; chan++) {
						myPCMData.push_back(floatToPCM(inbuffer[(samp * inchannels) + chan]));			//Adds sample to PCM buffer in approprate channel
#ifndef NDEBUG
						samplesAddedCounter++;
#endif
					}
				}
				break;

			//Add cases here for other input channel configurations. Ultimately, all must mix down to stereo.

			default:
				std::cout << "DSP needs mono or stereo in and out!" << std::endl;
				return FMOD_ERR_DSP_DONTPROCESS;
				break;
		}
	}
	return FMOD_ERR_DSP_SILENCE;		//ensures System output is silent without manually telling every sample to be 0.0f
}

// Callback that triggers when an Event Instance is released
// Despite the name, this callback will receive callbacks of all types, and so must filter them out
FMOD_RESULT F_CALLBACK eventInstanceDestroyedCallback(FMOD_STUDIO_EVENT_CALLBACK_TYPE type, FMOD_STUDIO_EVENTINSTANCE* event, void* parameters) {

	FMOD::Studio::EventInstance* myEvent = (FMOD::Studio::EventInstance*)event;		// Cast approved by Firelight in the documentation

	if (type == FMOD_STUDIO_EVENT_CALLBACK_DESTROYED) {								// Redundant due to callback mask
		std::cout << "Event Destroyed Callback Triggered." << std::endl;

		// Get some data to differentiate Events from Snapshots
		FMOD::Studio::EventDescription* myEventDesc = nullptr;
		myEvent->getDescription(&myEventDesc);
		bool isSnapshot = false;
		myEventDesc->isSnapshot(&isSnapshot);

		// Iterate through the respective Instance map and erase the entry associated with this Instance.
		if (!isSnapshot) {		// Event
			for (const auto& [niceName, sessionEventInstance] : pEventInstances) {
				if (myEvent == sessionEventInstance.instance) {
					std::cout << "Event Instance destroyed, erasing key from pEventInstances: " << niceName << std::endl;
					pEventInstances.erase(niceName);
				}
			}
		}
		else {					// Snapshot
			for (const auto& [niceName, snapshotInstance] : pSnapshotInstances) {
				if (myEvent == snapshotInstance) {
					std::cout << "Snapshot destroyed, erasing key from pSnapshotInstances: " << niceName << std::endl;
					pSnapshotInstances.erase(niceName);
				}
			}
		}
	}
	return FMOD_OK;
}

//---Bot Functions---//

// Simple ping, responds in chat and output log
void ping(const dpp::slashcommand_t& event) {
	event.reply(dpp::message("Pong! I'm alive!").set_flags(dpp::m_ephemeral));
	std::cout << "Responding to Ping command." << std::endl;
}

// Base function, called on startup and when requested by List Banks command
void banks() {

	std::cout << "Checking Banks path: " << bank_dir_path.string() << std::endl;
	std::string output = "- Master.bank\n- Master.strings.bank\n";				//Ensures Master and Strings banks are always in list

	//Form list of .bank files in the soundbanks folder
	for (const auto& entry : std::filesystem::directory_iterator(bank_dir_path)) {		// For every entry found in the banks folder
		if (entry.is_directory()) {														// Skip if directory...
			std::cout << "   Skipped: " << entry.path().string() << " -- is a directory." << std::endl;
			continue;
		}
		else if (entry.path().extension() != ".bank") {									// Skip if not a bank...
			std::cout << "   Skipped: " << entry.path().string() << " -- Extension is "
				<< entry.path().extension() << " which isn't an FMOD bank." << std::endl;
			continue;
		}
		else if ((entry.path().filename() == "Master.bank")								// Skip if Master or Strings...
			|| (entry.path().filename() == "Master.strings.bank")) {
			std::cout << "   Skipped: " << entry.path().string() << " -- is Master or Strings bank." << std::endl;
		}
		else {
			std::cout << "   Accepted: " << entry.path().string() << std::endl;			// Accepted!
			bankPaths.push_back(entry.path());
		}
	}

	//Get list of already loaded banks
	int count = 0;
	ERRCHECK_HARD(pSystem->getBankCount(&count));
	std::vector<FMOD::Studio::Bank*> loadedBanks; loadedBanks.resize(count);
	int writtenCount = 0;
	ERRCHECK_HARD(pSystem->getBankList(loadedBanks.data(), count, &writtenCount));

	// For every accepted bank path, add to output list, and load if not loaded already
	for (int i = 0; i < (int)bankPaths.size(); i++) {					// For every accepted bank path
		bool canLoad = true;
		// Check the bank isn't already loaded
		for (int j = 0; j < count; j++) {								// For each loaded bank
			char pathchars[256];						// Hope we don't need longer paths than this...
			char* pathptr = pathchars;
			int retrieved = 0;
			ERRCHECK_HARD(loadedBanks[j]->getPath(pathptr, 256, &retrieved));// Get the path as char*

			std::string pathString(pathptr);							// Make string, then format
			pathString = formatBankToFilepath(pathString, bank_dir_path);

			if (bankPaths[i].string() == pathString) {					// If the paths match, disqualify
				canLoad = false;
			}
		}

		if (canLoad) {
			FMOD::Studio::Bank* newBank = nullptr;						// Load, if bank qualifies
			ERRCHECK_HARD(pSystem->loadBankFile(bankPaths[i].string().c_str(), FMOD_STUDIO_LOAD_BANK_NORMAL, &newBank));
			pBanks.push_back(newBank);
			std::cout << "   Loaded: " << bankPaths[i].string() << std::endl;
		}
		else { std::cout << "   Skipped Load: " << bankPaths[i].string() << std::endl; }
	}

	// Cleanup! Unload unused banks
	int offset = 0;
	int i = 0;

	// This is probably going to break. Have to try it with a bunch of banks and remove a few.
	while (i < (int)pBanks.size()) {							// For each loaded bank

		bool foundInList = false;							// Get the path ("bank:/")
		char pathchars[256];
		char* pathptr = pathchars;
		int retrieved = 0;
		ERRCHECK_HARD(pBanks[i + offset]->getPath(pathptr, 256, &retrieved));

		std::string pathString(pathptr);					// Make string, then format to filepath
		pathString = formatBankToFilepath(pathString, bank_dir_path);

		for (int j = 0; j < (int)bankPaths.size(); j++) {		// For each Bank filepath we know of
			if (pathString == bankPaths[j]) {				// check if found in paths list
				foundInList = true;
			}
		}
		if (!foundInList) {									// If not found in list
			std::cout << "Unload & Delete: " << pathString << std::endl;
			pBanks[i + offset]->unload();					// Unload, then erase that element
			pBanks.erase(pBanks.begin() + i);
			offset--;
		}
		else { i++; }
		if (i >= INT16_MAX || offset <= INT16_MIN) {		// Emergency cutoff switch to prevent infinite loop...assuming no user will have 32767+ banks.
			break;											// Probably wasteful, but makes me sleep better at night.
		}
	}
}

// Indexes and Prints all valid .bank files
void banks(const dpp::slashcommand_t& event) {

	if (pEventInstances.size() > 0) {							// Unsafe to load/unload banks while events are active
		event.reply(dpp::message("Cannot mess with banks while the bot is playing audio! Please stop all events first.").set_flags(dpp::m_ephemeral));
		return;
	}
	
	bankPaths.clear();											// Clear current Bank vector

	//Show "Thinking..." while putting the list together
	event.thinking(true, [event](const dpp::confirmation_callback_t& callback) {

		banks();											// Re-list what banks exist, load new ones

		std::string output = "- Master.bank\n- Master.strings.bank\n";

		for (int i = 0; i < (int)bankPaths.size(); i++) {
			output.append("- " + bankPaths[i].filename().string() + "\n");
		}

		event.edit_original_response(dpp::message("## Found FMOD Banks: ##\n" + output));
	});
}

// Indexes all Events, Busses, VCAs, Snapshots, etc. on Startup ONLY.
void index() {
	// Make sure vectors and maps are clear
	eventPaths.clear();
	busPaths.clear();
	vcaPaths.clear();
	snapshotPaths.clear();

	int count = 0;
	ERRCHECK_HARD(pMasterStringsBank->getStringCount(&count));
	if (count <= 0) {
		std::cout << "Invalid strings count of " << count << ", that's a problem." << std::endl;
		std::cout << "Double check the Master.strings.bank file was loaded properly." << std::endl;
		return;
	}

	// Loop through every entry
	for (int i = 0; i < count; i++) {
		FMOD_GUID pathGUID;
		char pathStringChars[256];
		char* pathStringCharsptr = pathStringChars;
		int retreived;

		ERRCHECK_HARD(pMasterStringsBank->getStringInfo(i, &pathGUID, pathStringCharsptr, 256, &retreived));
		std::string pathString(pathStringCharsptr);

		// Is it an event?
		if ((pathString.find("event:/", 0) == 0)) {

			// Skip it if not in the Master folder.
			if ((pathString.find(callableEventPath, 0) != 0)) {
				std::cout << "   Skipped as Event: " << pathString << " -- Not in Master folder." << std::endl;
				continue;
			}

			// What's left should be good for our eventPaths vector
			std::cout << "   Accepted as Event: " << pathString << std::endl;
			eventPaths.push_back(pathString);

			// Grab associated Event Description
			FMOD::Studio::EventDescription* newEventDesc = nullptr;
			ERRCHECK_HARD(pSystem->getEvent(pathString.c_str(), &newEventDesc));
			sessionEventDesc newSessionEventDesc; newSessionEventDesc.description = newEventDesc;

			// Grab the name of each associated non-built-in parameter
			int descParamCount = 0;
			newSessionEventDesc.description->getParameterDescriptionCount(&descParamCount);

			for (int i = 0; i < descParamCount; i++) {
				FMOD_STUDIO_PARAMETER_DESCRIPTION parameter;
				newSessionEventDesc.description->getParameterDescriptionByIndex(i, &parameter);
				if (parameter.type == FMOD_STUDIO_PARAMETER_GAME_CONTROLLED) {
					std::string paramName(parameter.name);
					std::string coutString = "      ";

					coutString.append(" - Parameter: " + paramName);
					coutString.append(" " + paramMinMaxString(parameter));
					coutString.append(" " + paramAttributesString(parameter));
#ifndef NDEBUG
					coutString.append(" with flag: " + std::to_string(parameter.flags));
#endif
					std::cout << coutString;
					newSessionEventDesc.params.push_back(parameter);
				}
			}
			pEventDescriptions.insert({ truncateEventPath(pathString), newSessionEventDesc });	// Add to map, connected to a trimmed "easy" path name
		}

		// Is it a bus?
		else if ((pathString.find(busPath, 0) == 0)) {
			if (pathString == busPath) {		//The Master Bus is just "bus:/"

				std::cout << "   Accepted as Master Bus: " << pathString << std::endl;
			}
			else {
				// Get the Bus and add it to the map
				FMOD::Studio::Bus* newBus = nullptr;
				ERRCHECK_HARD(pSystem->getBus(pathString.c_str(), &newBus));
				pBusses.insert({ truncateBusPath(pathString), newBus });
				busPaths.push_back(pathString);
				std::cout << "   Accepted as Bus: " << pathString << " || Nice Name: " << truncateBusPath(pathString) << std::endl;
			}
		}

		// Is it a VCA?
		else if ((pathString.find(vcaPath, 0) == 0)) {
			// Get the VCA and add it to the map
			FMOD::Studio::VCA* newVCA = nullptr;
			ERRCHECK_HARD(pSystem->getVCA(pathString.c_str(), &newVCA));
			pVCAs.insert({ truncateVCAPath(pathString), newVCA });

			vcaPaths.push_back(pathString);
			std::cout << "   Accepted as VCA: " << pathString << " || Nice Name: " << truncateVCAPath(pathString) << std::endl;
		}

		// Is it a Snapshot?
		else if ((pathString.find(snapshotPath, 0) == 0)) {

			// Get the Snapshot and add it to the map
			FMOD::Studio::EventDescription* newSnapshot = nullptr;
			ERRCHECK_HARD(pSystem->getEvent(pathString.c_str(), &newSnapshot));

			bool isSnapshot = false;
			newSnapshot->isSnapshot(&isSnapshot);
			if (!isSnapshot) {		//If this event description isn't actually a Snapshot
				std::cout << "   Skipped as Snapshot: " << pathString << " -- Not actually a snapshot!" << std::endl;
			}
			else {
				pSnapshots.insert({ truncateSnapshotPath(pathString), newSnapshot });
				snapshotPaths.push_back(pathString);
				std::cout << "   Accepted as Snapshot: " << pathString << " || Nice Name: " << truncateSnapshotPath(pathString) << std::endl;
			}
		}

		// Is it a parameter? (todo: something with this, so we can possibly set global parameters)
		else if (pathString.find("parameter:/") == 0) {
			std::cout << "   Skipped as Parameter: " << pathString << " -- Still working on indexing params!" << std::endl;
		}

		// Is it a bank? (We don't care here, just for Cout data)
		else if (pathString.find("bank:/", 0) == 0) {
			std::cout << "   Skipped as Bank: " << pathString << " -- Is a Bank." << std::endl;
		}
		
		// If it's none of the above, then we have NO idea what this thing is.
		else { std::cout << "   Skipped: " << pathString << " -- Unrecognized string." << std::endl; }
	}
}

// Base function, called on startup and when requested by List Events command
void events() {
	int count = 0;
	ERRCHECK_HARD(pMasterStringsBank->getStringCount(&count));
	if (count <= 0) {
		std::cout << "Invalid strings count of " << count << ", that's a problem." << std::endl;
		std::cout << "Double check the Master.strings.bank file was loaded properly." << std::endl;
		return;
	}

	for (int i = 0; i < count; i++) {
		FMOD_GUID pathGUID;
		char pathStringChars[256];
		char* pathStringCharsptr = pathStringChars;
		int retreived;

		ERRCHECK_HARD(pMasterStringsBank->getStringInfo(i, &pathGUID, pathStringCharsptr, 256, &retreived));
		std::string pathString(pathStringCharsptr);

		// Discard all strings that aren't events in the Master folder (busses, VCAs, Parameters, other events, etc.)
		if ((pathString.find("event:/", 0) != 0)) {										// Skip if not Event...
			std::cout << "   Skipped: " << pathString << " -- Not Event." << std::endl;
			continue;
		}
		if ((pathString.find(callableEventPath, 0) != 0)) {								// Skip if not in Master folder...
			std::cout << "   Skipped: " << pathString << " -- Not in Master folder." << std::endl;
			continue;
		}
		if (!eventPaths.empty()) {
			bool alreadyExists = false;
			for (int i = 0; i < eventPaths.size(); i++) {
				if (eventPaths[i] == pathString) { alreadyExists = true; }
			}
			if (alreadyExists) {
				std::cout << "   Skipped: " << pathString << " -- Already Listed." << std::endl;
				continue;
			}
		}

		// What's left should be good for our eventPaths vector
		std::cout << "   Accepted as Event: " << pathString << std::endl;
		eventPaths.push_back(pathString);

		// Grab associated Event Description
		FMOD::Studio::EventDescription* newEventDesc = nullptr;
		ERRCHECK_HARD(pSystem->getEvent(pathString.c_str(), &newEventDesc));
		sessionEventDesc newSessionEventDesc; newSessionEventDesc.description = newEventDesc;

		// Grab the name of each associated non-built-in parameter
		int descParamCount = 0;
		newSessionEventDesc.description->getParameterDescriptionCount(&descParamCount);
		for (int i = 0; i < descParamCount; i++) {
			FMOD_STUDIO_PARAMETER_DESCRIPTION parameter;
			newSessionEventDesc.description->getParameterDescriptionByIndex(i, &parameter);
			if (parameter.type == FMOD_STUDIO_PARAMETER_GAME_CONTROLLED) {
				std::string paramName(parameter.name);
				std::string coutString = "      ";

				coutString.append(" - Parameter: " + paramName);
				coutString.append(" " + paramMinMaxString(parameter));
				coutString.append(" " + paramAttributesString(parameter));
#ifndef NDEBUG
				coutString.append(" with flag: " + std::to_string(parameter.flags));
#endif
				std::cout << coutString;
				newSessionEventDesc.params.push_back(parameter);
			}
		}
		pEventDescriptions.insert({ truncateEventPath(pathString), newSessionEventDesc });		// Add to map, connected to a trimmed "easy" path name
	}
}

// Indexes and Prints all playable Event Descriptions & Parameters
void events(const dpp::slashcommand_t& event) {

	if (!pMasterStringsBank->isValid() || pMasterStringsBank == nullptr) {
		std::cout << "Master Strings bank is invalid or nullptr. Bad juju!" << std::endl;
		event.reply(dpp::message("Master Strings bank is invalid or nullptr. Bad juju!")
			.set_flags(dpp::m_ephemeral));
		return;
	}

	eventPaths.clear();
	pEventDescriptions.clear();

	event.thinking(true, [event](const dpp::confirmation_callback_t& callback) {

		// Index the events with function above
		events();

		// And now print 'em to Discord!
		
		if (eventPaths.size() == 0) {
			event.edit_original_response(dpp::message("No playable events found!"));		// If no events were found _at all_ then say so.
		}
		else {
			dpp::embed paramListEmbed = basicEmbed;								// Create the embed and set non-standard details
			paramListEmbed.set_title("Event List with Params");

			for (int i = 0; i < (int)eventPaths.size(); i++) {										// For every path	
				std::vector<FMOD_STUDIO_PARAMETER_DESCRIPTION> eventParams = pEventDescriptions.at(truncateEventPath(eventPaths[i])).params;
				if (eventParams.size() == 0) {
					paramListEmbed.add_field(truncateEventPath(eventPaths[i]), "");			// Add the shortened path as a field		
					continue;
				}
				else {
					std::string paramOutString = "";
					for (int j = 0; j < (int)eventParams.size(); j++) {			// as well as each associated parameters and their ranges, if any.
						paramOutString.append(eventParams[j].name);
						paramOutString.append(paramMinMaxString(eventParams[j]) + paramAttributesString(eventParams[j]));
					}
					paramListEmbed.add_field(truncateEventPath(eventPaths[i]), paramOutString);
				}
			}
			event.edit_original_response(dpp::message(event.command.channel_id, paramListEmbed));
		}
	});
}

// Prints all currently playing Event Instances
void list(const dpp::slashcommand_t& event) {
	// Simply list the currently playing events. No indexing here.
	// Todo: show the parameters of each playing event, eventually.

	event.thinking(true, [event](const dpp::confirmation_callback_t& callback) {

		if (pEventInstances.empty()) {
			std::cout << "No playing Event Instances." << std::endl;
			event.edit_original_response(dpp::message("No playing Event Instances."));
		}
		else {
			dpp::embed paramListEmbed = basicEmbed;								// Create the embed and set non-standard details
			paramListEmbed.set_title("Playing Events");

			for (auto const& inst : pEventInstances) {		// For each Event Instance

				// Get the Event Instance name
				std::string instName = inst.first;

				// Get the related Event Description
				FMOD::Studio::EventDescription* instDesc;
				inst.second.instance->getDescription(&instDesc);

				// Get the Event Description's name
				char pathchars[256];						// Hope we don't need longer paths than this...
				char* pathptr = pathchars;
				int retrieved = 0;
				ERRCHECK_HARD(instDesc->getPath(pathptr, 512, &retrieved));	// Get the path as char*
				std::string instDescName(pathptr);							// Make string, then format
				instDescName = truncateEventPath(instDescName);
				
				if (inst.second.params.empty()) {							// If no parameters,
					paramListEmbed.add_field(instName, instDescName);		// add field with the name and Desc path
				}
				else {														// If there ARE parameters, strap in...
					std::string paramOutString = "";
					if (instDescName != instName) {				// If the instance and event name are different
						paramOutString.append("event: " + instDescName + "\n");	// add in the event name
					}
					for (int i = 0; i < inst.second.params.size(); i++) {

						// get current parameter name & value
						std::string paramName(inst.second.params[i].name);
						float paramVal = 0; float paramFinalVal = 0;
						ERRCHECK_HARD(inst.second.instance->getParameterByID(inst.second.params[i].id, &paramVal, &paramFinalVal));
						std::string paramValStr = paramValueString(paramVal, inst.second.params[i]);

						// Get the min/max
						std::string paramMinMaxStr = paramMinMaxString(inst.second.params[i]);
						// Get the Attributes
						std::string paramAttributesStr = paramAttributesString(inst.second.params[i]);

						// Glue 'em all together, adding new lines per-parameter
						paramOutString.append("- " + paramName + ": " + paramValStr + "  " + paramMinMaxStr + paramAttributesStr);
						if (i > 0) { paramOutString.append("\n"); }
					}
					paramListEmbed.add_field(instName, paramOutString);
				}
			}

			event.edit_original_response(dpp::message(event.command.channel_id, paramListEmbed));
		}
	});
}

// Creates and starts a new Event Instance
void play(const dpp::slashcommand_t& event) {

	dpp::command_interaction cmd_data = event.command.get_command_interaction();
	int count = (int)cmd_data.options.size();
	if (count < 1) {
		std::cout << "Play command arrived with no arguments. Bad juju!" << std::endl;
		event.reply(dpp::message("Play command sent with no arguments. Bad juju!").set_flags(dpp::m_ephemeral));
		return;
	}
	else if (count > 2) {
		std::cout << "Play command arrived with too many arguments (>2). Bad juju!" << std::endl;
		event.reply(dpp::message("Play command sent with too many arguments (>2). Bad juju!").set_flags(dpp::m_ephemeral));
		return;
	}

	std::string eventToPlay = std::get<std::string>(event.get_parameter(cmd_data.options[0].name));

	// Determining the Instance name
	// If the user gave a name in the command, use that
	// If the user gave no name, use the Event name
	std::string inputName;
	if (count > 1) {
		inputName = std::get<std::string>(event.get_parameter(cmd_data.options[1].name));
		if (inputName == "" || inputName.size() == 0) {		// If the input was bogus, pretend there was no input
			inputName = eventToPlay;
		}
	}
	else {
		inputName = eventToPlay;
	}

	std::string newName = inputName;
	std::string cleanName = newName;
	int iterator = 1;
	while (pEventInstances.find(newName) != pEventInstances.end()) {		// If that name already exists, quietly give it a number
		newName = cleanName + "-" + std::to_string(iterator);				// Keep counting up until valid.
		iterator++;
	}

	std::cout << "Play command issued." << std::endl;
	std::cout << "Event to Play: " << eventToPlay << " || Instance name: " << newName << std::endl;

	FMOD::Studio::EventDescription* newEventDesc = nullptr;// = pEventDescriptions.at(eventToPlay).description;
	bool isSnapshot = false;

	if (pEventDescriptions.contains(eventToPlay)) {
		newEventDesc = pEventDescriptions.at(eventToPlay).description;
	}
	else if (pSnapshots.contains(eventToPlay)) {
		newEventDesc = pSnapshots.at(eventToPlay);
		newEventDesc->isSnapshot(&isSnapshot);
	}

	if ((newEventDesc != nullptr) && (newEventDesc->isValid())) {
		if (!isSnapshot) {
			FMOD::Studio::EventInstance* newEventInst = nullptr;
			ERRCHECK_HARD(newEventDesc->createInstance(&newEventInst));
			std::vector<FMOD_STUDIO_PARAMETER_DESCRIPTION> newEventParams = pEventDescriptions.at(eventToPlay).params;
			sessionEventInstance newSessionEventInst;
			newSessionEventInst.instance = newEventInst;
			newSessionEventInst.params = newEventParams;
			pEventInstances.insert({ newName, newSessionEventInst });
			ERRCHECK_HARD(pEventInstances.at(newName).instance->setCallback(eventInstanceDestroyedCallback, FMOD_STUDIO_EVENT_CALLBACK_DESTROYED));
			ERRCHECK_HARD(pEventInstances.at(newName).instance->start());
			ERRCHECK_HARD(pEventInstances.at(newName).instance->release());

			std::cout << "Playing event: " << eventToPlay << " with Instance name: " << newName << std::endl;
			event.reply(dpp::message("Playing event: " + eventToPlay + " with Instance name: " + newName).set_flags(dpp::m_ephemeral));
		}
		else {
			FMOD::Studio::EventInstance* newSnapInst = nullptr;
			ERRCHECK_HARD(newEventDesc->createInstance(&newSnapInst));
			ERRCHECK_HARD(newSnapInst->setCallback(eventInstanceDestroyedCallback, FMOD_STUDIO_EVENT_CALLBACK_DESTROYED));
			ERRCHECK_HARD(newSnapInst->start());
			ERRCHECK_HARD(newSnapInst->release());
			pSnapshotInstances.insert({ newName, newSnapInst });

			std::cout << "Playing snapshot: " << eventToPlay << " with Instance name: " << newName << std::endl;
			event.reply(dpp::message("Playing snapshot: " + eventToPlay + " with Instance name: " + newName).set_flags(dpp::m_ephemeral));
		}
	}
	else {
		std::cout << "No valid Event or Snapshot found with the given path." << std::endl;
		event.reply(dpp::message("No valid Event or Snapshot found with the given path.").set_flags(dpp::m_ephemeral));
	}
}

// Pauses Event with given name in events playing list
void pause(const dpp::slashcommand_t& event) {
	//Very similar to Unpause and Stop
	dpp::command_interaction cmd_data = event.command.get_command_interaction();
	int count = (int)cmd_data.options.size();
	if (count < 1) {
		std::cout << "Pause command arrived with no arguments. Bad juju!" << std::endl;
		event.reply(dpp::message("Pause command sent with no arguments. Bad juju!").set_flags(dpp::m_ephemeral));
		return;
	}

	std::string inputName = std::get<std::string>(event.get_parameter(cmd_data.options[0].name));

	std::cout << "Pause command issued." << std::endl;
	std::cout << "Instance Name: " << inputName << std::endl;
	if (pEventInstances.find(inputName) != pEventInstances.end()) {
		pEventInstances.at(inputName).instance->setPaused(true);
		std::cout << "Pause command carried out." << std::endl;
		event.reply(dpp::message("Pausing Event Instance: " + inputName).set_flags(dpp::m_ephemeral));
	}
	else if (pSnapshotInstances.find(inputName) != pSnapshotInstances.end()) {
		pSnapshotInstances.at(inputName)->setPaused(true);
		std::cout << "Pause command carried out." << std::endl;
		event.reply(dpp::message("Pausing Snapshot: " + inputName).set_flags(dpp::m_ephemeral));
	}
	else {
		std::cout << "Couldn't find Instance with given name." << std::endl;
		event.reply(dpp::message("No Event Instance found with given name: " + inputName).set_flags(dpp::m_ephemeral));
	}
}

void unpause(const dpp::slashcommand_t& event) {
	//Very similar to Pause and Stop
	dpp::command_interaction cmd_data = event.command.get_command_interaction();
	int count = (int)cmd_data.options.size();
	if (count < 1) {
		std::cout << "Unpause command arrived with no arguments. Bad juju!" << std::endl;
		event.reply(dpp::message("Unpause command sent with no arguments. Bad juju!").set_flags(dpp::m_ephemeral));
		return;
	}

	std::string inputName = std::get<std::string>(event.get_parameter(cmd_data.options[0].name));

	std::cout << "Unpause command issued." << std::endl;
	std::cout << "Instance Name: " << inputName << std::endl;
	if (pEventInstances.find(inputName) != pEventInstances.end()) {
		pEventInstances.at(inputName).instance->setPaused(false);
		std::cout << "Unpause command carried out." << std::endl;
		event.reply(dpp::message("Unpausing Event Instance: " + inputName).set_flags(dpp::m_ephemeral));
	}
	else if (pSnapshotInstances.find(inputName) != pSnapshotInstances.end()) {
		pSnapshotInstances.at(inputName)->setPaused(false);
		std::cout << "Unpause command carried out." << std::endl;
		event.reply(dpp::message("Unpausing Snapshot: " + inputName).set_flags(dpp::m_ephemeral));
	}
	else {
		std::cout << "Couldn't find Instance with given name." << std::endl;
		event.reply(dpp::message("No Event Instance found with given name: " + inputName).set_flags(dpp::m_ephemeral));
	}
}

// Stops Event or Snapshot with given name in events playing list
void stop(const dpp::slashcommand_t& event) {
	// Very similar to Pause and Unpause
	dpp::command_interaction cmd_data = event.command.get_command_interaction();
	int count = (int)cmd_data.options.size();
	if (count < 1) {			// If somehow this was sent without arguments, that's bad.
		std::cout << "Stop command arrived with no arguments. Bad juju!" << std::endl;
		event.reply(dpp::message("Stop command sent with no arguments. Bad juju!").set_flags(dpp::m_ephemeral));
		return;
	}

	std::cout << "Stop command issued." << std::endl;
	std::string inputName = std::get<std::string>(event.get_parameter(cmd_data.options[0].name));
	bool value;
	if (count > 1) { value = std::get<bool>(event.get_parameter(cmd_data.options[2].name)); }
	else { value = false; }

	std::cout << "Instance Name: " << inputName << std::endl;
	if (pEventInstances.find(inputName) != pEventInstances.end()) {
		if (value) { pEventInstances.at(inputName).instance->stop(FMOD_STUDIO_STOP_ALLOWFADEOUT); }
		else { pEventInstances.at(inputName).instance->stop(FMOD_STUDIO_STOP_IMMEDIATE); }
		//Callback should handle removing this instance from our map when the event is done.
		std::cout << "Stop command carried out." << std::endl;
		event.reply(dpp::message("Stopping Event Instance: " + inputName).set_flags(dpp::m_ephemeral));
	}
	else if (pSnapshotInstances.find(inputName) != pSnapshotInstances.end()) {
		if (value) { pSnapshotInstances.at(inputName)->stop(FMOD_STUDIO_STOP_ALLOWFADEOUT); }
		else { pSnapshotInstances.at(inputName)->stop(FMOD_STUDIO_STOP_IMMEDIATE); }
		std::cout << "Stop command carried out." << std::endl;
		event.reply(dpp::message("Stopping Snapshot: " + inputName).set_flags(dpp::m_ephemeral));
	}
	else {
		std::cout << "Couldn't find Instance with given name." << std::endl;
		event.reply(dpp::message("No Event Instance or Snapshot found with given name: " + inputName).set_flags(dpp::m_ephemeral));
	}
	
}

// Base function, called in a few places as part of other methods like quit()
void stopallevents() {
	std::cout << "Stopping all events...";
	//For each instance in events playing list, stop_now
	for (const auto& [niceName, sessionEventInstance] : pEventInstances) {
		sessionEventInstance.instance->stop(FMOD_STUDIO_STOP_IMMEDIATE);
	}
	std::cout << "Done." << std::endl;
}

// Base function, stops all snapshots.
void stopallsnaps() {
	std::cout << "Stopping Snapshots...";
	for (const auto& [niceName, snapshotInstance] : pSnapshotInstances) {
		snapshotInstance->stop(FMOD_STUDIO_STOP_IMMEDIATE);
	}
	std::cout << "Done." << std::endl;
}

// Stops all playing events and/or snapshots in the list.
void stopall(const dpp::slashcommand_t& event) {
	stopallevents();
	stopallsnaps();
	event.reply(dpp::message("All events stopped.").set_flags(dpp::m_ephemeral));
}

// Sets parameter with given name and value on given Event Instance
void param(const dpp::slashcommand_t& event) {
	dpp::command_interaction cmd_data = event.command.get_command_interaction();
	int count = (int)cmd_data.options.size();
	if (count < 3) {
		std::cout << "Set Parameter command arrived with no arguments. Bad juju!" << std::endl;
		event.reply(dpp::message("Set Parameter command sent with no arguments. Bad juju!").set_flags(dpp::m_ephemeral));
	}

	std::cout << "Set Parameter command issued." << std::endl;
	std::string instanceName = std::get<std::string>(event.get_parameter(cmd_data.options[0].name));
	std::string paramName = std::get<std::string>(event.get_parameter(cmd_data.options[1].name));
	float value = (float)std::get<double>(event.get_parameter(cmd_data.options[2].name));

	std::cout << "Instance Name: " << instanceName << std::endl;
	if (pEventInstances.find(instanceName) == pEventInstances.end()) {
		std::cout << "Couldn't find Instance with given name." << std::endl;
		event.reply(dpp::message("No Event Instance found with given name: " + instanceName).set_flags(dpp::m_ephemeral));
	}
	else if (pEventInstances.at(instanceName).params.size() == 0) {
		// Todo: check here for Global parameters as well, just to be sure.
		std::cout << "Instance has no parameters." << std::endl;
		event.reply(dpp::message("Instance " + instanceName + " has no parameters associated with it.").set_flags(dpp::m_ephemeral));
	}	// In future, perhaps another else if to filter out the type, if we want to handle that in a special way

	int foundParamIndex = -1;
	for (int i = 0; i < (int)pEventInstances.at(instanceName).params.size(); i++) {
		if (pEventInstances.at(instanceName).params[i].name == paramName) {
			foundParamIndex = i;
		}
	}
	if (foundParamIndex < 0) {
		std::cout << "Parameter with that name couldn't be found." << std::endl;
		event.reply(dpp::message("Instance " + instanceName + " has no parameters of name " + paramName + " associated with it.").set_flags(dpp::m_ephemeral));
	}

	// Finally set the parameter
	ERRCHECK_HARD(pEventInstances.at(instanceName).instance->setParameterByName(paramName.c_str(), value));
	std::cout << "Command carried out." << std::endl;
	event.reply(dpp::message("Setting Parameter: " + paramName + " on Instance " + instanceName + " with value " + std::to_string(value)).set_flags(dpp::m_ephemeral));
}

// Sets the volume of a given Bus or VCA
void volume(const dpp::slashcommand_t& event) {
	dpp::command_interaction cmd_data = event.command.get_command_interaction();
	int count = (int)cmd_data.options.size();
	if (count < 2) {
		std::cout << "Set Volume command arrived with improper arguments. Bad juju!" << std::endl;
		event.reply(dpp::message("Set Volume command arrived with improper arguments. Bad juju!").set_flags(dpp::m_ephemeral));
	}

	std::cout << "Set Volume command issued." << std::endl;
	std::string busOrVCAName = std::get<std::string>(event.get_parameter(cmd_data.options[0].name));
	float value = (float)std::get<double>(event.get_parameter(cmd_data.options[1].name));
	if (value > 10.0f) { value *= -1; }
	value = dBToFloat(value);

	// If found in Busses map
	if (pBusses.find(busOrVCAName) != pBusses.end()) {
		ERRCHECK_HARD(pBusses.at(busOrVCAName)->setVolume(value));
		event.reply(dpp::message("Setting Bus: " + busOrVCAName + " to volume: " + std::to_string(floatTodB(value))).set_flags(dpp::m_ephemeral));
	}
	// Else if "Master" (not kept in Busses map)
	else if (busOrVCAName == "Master" || busOrVCAName == "master") {
		ERRCHECK_HARD(pMasterBus->setVolume(value + DISCORD_MASTERBUS_VOLOFFSET));
		event.reply(dpp::message("Setting Bus: Master to volume: " + std::to_string(floatTodB(value))).set_flags(dpp::m_ephemeral));
	}
	// Else if found in VCA map
	else if (pVCAs.find(busOrVCAName) != pVCAs.end()) {
		ERRCHECK_HARD(pVCAs.at(busOrVCAName)->setVolume(value));
		event.reply(dpp::message("Setting VCA: " + busOrVCAName + " to volume: " + std::to_string(floatTodB(value))).set_flags(dpp::m_ephemeral));
	}
}

void join(const dpp::slashcommand_t& event) {
	dpp::guild* guild = dpp::find_guild(event.command.guild_id);						//Get the Guild aka Server
	dpp::voiceconn* currentVC = event.from->get_voice(event.command.guild_id);			//Get the bot's current voice channel
	bool join_vc = true;
	if (currentVC) {																	//If already in voice...
		auto users_vc = guild->voice_members.find(event.command.get_issuing_user().id);	//get channel of user
		if ((users_vc != guild->voice_members.end()) && (currentVC->channel_id == users_vc->second.channel_id)) {
			join_vc = false;			//skips joining a voice chat below
		}
		else {
			event.from->disconnect_voice(event.command.guild_id);						//We're in a different VC, so leave it and join the new one below
			join_vc = true;																//possibly redundant assignment?
		}
	}
	if (join_vc) {																		//If we need to join a call above...
		if (!guild->connect_member_voice(event.command.get_issuing_user().id)) {		//try to connect, return false if we fail
			event.reply(dpp::message("You're not in a voice channel to be joined!").set_flags(dpp::m_ephemeral));
			std::cout << "Not in a voice channel to be joined." << std::endl;
			return;
		}
		//If not caught above, we're in voice! Not instant, will need to wait for on_voice_ready callback
		std::cout << "Joined channel of user." << std::endl;
		event.reply(dpp::message("Joined your voice channel!").set_flags(dpp::m_ephemeral));

	}
	else {
		event.reply(dpp::message("I am already living in your walls.").set_flags(dpp::m_ephemeral));
		std::cout << "Already living in your walls." << std::endl;
	}
}

void leave(const dpp::slashcommand_t& event) {
	dpp::voiceconn* currentVC = event.from->get_voice(event.command.guild_id);
	if (currentVC) {
		std::cout << "Leaving voice channel." << std::endl;
		stopallevents();										// Stop all FMOD events immediately
		stopallsnaps();

		isConnected = false;
		currentClient->stop_audio();
		currentClient = nullptr;

		event.from->disconnect_voice(event.command.guild_id);	// Disconnect from Voice (triggers callback laid out in main)
		event.reply(dpp::message("Bye bye! I hope I played good sounds!").set_flags(dpp::m_ephemeral));
	}
}

void quit(const dpp::slashcommand_t& event) {
	if (isConnected && (currentClient != nullptr)) {
		leave(event);
	}
	// Todo: clear D++ audio buffers and stop sending?
	// How does this overlap with the onDisconnect callback?
	exitRequested = true;
	std::cout << "Quit Command received." << std::endl;

}

// Initialize FMOD, DSP, and filepaths for later reference
void init() {
	std::cout << "###########################" << std::endl;
	std::cout << "###                     ###" << std::endl;
	std::cout << "###  Session Music Bot  ###" << std::endl;
	std::cout << "###                     ###" << std::endl;
	std::cout << "###########################" << std::endl;
	std::cout << std::endl;

	// file paths
	exe_path = getExecutableFolder();						// Special function from SessionMusicBot_Utils.h
	bank_dir_path = exe_path.append("soundbanks");			// Todo: validate this path exists

	// FMOD Init
	std::cout << "Initializing FMOD...";
	ERRCHECK_HARD(FMOD::Studio::System::create(&pSystem));
	ERRCHECK_HARD(pSystem->getCoreSystem(&pCoreSystem));
	//ERRCHECK_HARD(pCoreSystem->setDSPBufferSize(4096, 4));
	ERRCHECK_HARD(pSystem->initialize(128, FMOD_STUDIO_INIT_NORMAL, FMOD_INIT_NORMAL, nullptr));
	std::cout << "Done." << std::endl;

	// Load Master Bank and Master Strings
	std::cout << "Loading Master banks...\n";
	ERRCHECK_HARD(pSystem->loadBankFile((bank_dir_path.string() + "\\" + master_bank).c_str(), FMOD_STUDIO_LOAD_BANK_NORMAL, &pMasterBank));
	ERRCHECK_HARD(pSystem->loadBankFile((bank_dir_path.string() + "\\" + masterstrings_bank).c_str(), FMOD_STUDIO_LOAD_BANK_NORMAL, &pMasterStringsBank));
	std::cout << "Done." << std::endl;

	// Also get the Master Bus, set volume, and get the related Channel Group
	std::cout << "Getting Busses and Channel Groups...";
	ERRCHECK_HARD(pSystem->getBus("bus:/", &pMasterBus));
	ERRCHECK_HARD(pMasterBus->setVolume(dBToFloat(DISCORD_MASTERBUS_VOLOFFSET)));
	ERRCHECK_HARD(pMasterBus->lockChannelGroup());					// Tell the Master Channel Group to always exist even when events arn't playing...
	ERRCHECK_HARD(pSystem->flushCommands());							// And wait until all previous commands are done (ensuring Channel Group exists)...
	ERRCHECK_HARD(pMasterBus->getChannelGroup(&pMasterBusGroup));	// Or else this fails immediately, and we'll have DSP problems.
	std::cout << "Done." << std::endl;
	

	// Define and create our capture DSP on the Master Channel Group.
	// Copied from FMOD's examples, unsure why this works and why it must be in brackets.
	{
		FMOD_DSP_DESCRIPTION dspdesc;
		memset(&dspdesc, 0, sizeof(dspdesc));
		strncpy_s(dspdesc.name, "LH_captureDSP", sizeof(dspdesc.name));
		dspdesc.version = 0x00010000;
		dspdesc.numinputbuffers = 1;
		dspdesc.numoutputbuffers = 1;
		dspdesc.read = captureDSPReadCallback;
		ERRCHECK_HARD(pCoreSystem->createDSP(&dspdesc, &mCaptureDSP));
	}
	ERRCHECK_HARD(pMasterBusGroup->addDSP(FMOD_CHANNELCONTROL_DSP_TAIL, mCaptureDSP));		// Adds the newly defined dsp

	// Setting Listener positioning for 3D, in case it's used 
	std::cout << "Setting up Listener...";
	listenerAttributes.position = { 0.0f, 0.0f, 0.0f };
	listenerAttributes.forward = { 0.0f, 1.0f, 0.0f };
	listenerAttributes.up = { 0.0f, 0.0f, 1.0f };
	ERRCHECK_HARD(pSystem->setListenerAttributes(0, &listenerAttributes));
	std::cout << "Done." << std::endl;

	// Debug details
	int samplerate; FMOD_SPEAKERMODE speakermode; int numrawspeakers;
	ERRCHECK_HARD(pCoreSystem->getSoftwareFormat(&samplerate, &speakermode, &numrawspeakers));
	ERRCHECK_HARD(pSystem->flushCommands());
	std::cout << std::endl;
	std::cout << "###########################" << std::endl << std::endl;
	std::cout << "FMOD System Info:\n  Sample Rate- " << samplerate << "\n  Speaker Mode- " << speakermode
		<< "\n  Num Raw Speakers- " << numrawspeakers << std::endl;
	std::cout << std::endl;
}

// Init function specifically for user-defined needs (loading banks, indexing events & params, etc)
void init_session() {
	std::cout << "###########################" << std::endl << std::endl;
	std::cout << "Loading and Indexing all other banks..." << std::endl;
	banks();
	std::cout << "...Done!" << std::endl << std::endl;

	std::cout << "Indexing Objects..." << std::endl;
	index();
	std::cout << "...Done!" << std::endl << std::endl;
	std::cout << "###########################" << std::endl;
	std::cout << std::endl;
}

int main() {

	init();
	init_session();

#ifndef NDEBUG
	//Time stuff for debugging
	std::chrono::system_clock::time_point start = std::chrono::system_clock::now();
	std::chrono::system_clock::time_point end = std::chrono::system_clock::now();
	std::chrono::system_clock::time_point last = end;
	std::chrono::duration<double, std::milli> elapsed;
	std::chrono::duration<double, std::milli> elapsedFrame;

	bool timerNotRunning = true;		//To make sure our times start at the first update loop where we know we've connected
	int allSamplesAdded = 0;
#endif
	std::cout << "Starting Bot..." << std::endl << std::endl;

	/* Create bot cluster */
	dpp::cluster bot(getBotToken());

	/* Output simple log messages to stdout */
	bot.on_log(dpp::utility::cout_logger());

	// Get the bot application, and add the Owner to the Owning Users list (for permissions)
	dpp::application botapp = bot.current_application_get_sync();	//Blocking function used for simplicity
	std::cout << "Owner Username: " << botapp.owner.username << " with Snowflake ID: " << botapp.owner.id << std::endl;
	owningUsers.push_back(botapp.owner.id);

	/* Register slash command here in on_ready */
	bot.on_ready([&bot](const dpp::ready_t& event) {
		/* Wrap command registration in run_once to make sure it doesnt run on every full reconnection */
		if (dpp::run_once<struct register_bot_commands>()) {
			std::vector<dpp::slashcommand> commands {
				{ "events", "List all playable events and their parameters.", bot.me.id},
				{ "list", "Show all playing event instances and their parameters.", bot.me.id},
				{ "play", "Create a new Event Instance (or Snapshot).", bot.me.id},
				{ "pause", "Pause a currently playing Event Instance.", bot.me.id},
				{ "unpause", "Resume a currently playing Event Instance.", bot.me.id},
				{ "stop", "Stop a currently playing Event Instance.", bot.me.id},
				{ "stopall", "Stop all Event Instances and Snapshots immediately.", bot.me.id},
				{ "param", "Set a parameter on an Event Instance.", bot.me.id},
				{ "volume", "Set the volume of a bus or VCA.", bot.me.id},
				{ "ping", "Ping the bot to ensure it's alive.", bot.me.id },
				{ "banks", "List all banks in the Soundbanks folder.", bot.me.id},
				{ "join", "Join your current voice channel.", bot.me.id},
				{ "leave", "Leave the current voice channel.", bot.me.id},
				{ "quit", "Leave voice and exit the program.", bot.me.id}
			};

			// Sub-commands for Play
			commands[2].add_option(
				dpp::command_option(dpp::co_string, "event-name", "The Event (or Snapshot) you wish to play.", true)
			);
			commands[2].add_option(
				dpp::command_option(dpp::co_string, "instance-name", "Optional: name used for interactions with this new Instance. Defaults to the name of the Event.", false)
			);

			// Sub-commands for Pause
			commands[3].add_option(
				dpp::command_option(dpp::co_string, "instance-name", "The name of the Instance to pause.", true)
			);

			// Sub-commands for Unpause
			commands[4].add_option(
				dpp::command_option(dpp::co_string, "instance-name", "The name of the Instance to unpause.", true)
			);

			// Sub-commands for Stop
			commands[5].add_option(
				dpp::command_option(dpp::co_string, "instance-name", "The name of the Instance to stop.", true)
			);
			commands[5].add_option(
				dpp::command_option(dpp::co_boolean, "stop-immediately", "Optional: stop the Instance NOW, without fadeouts?", false)
			);

			// Sub-commands for Stop_All
			commands[6].add_option(
				dpp::command_option(dpp::co_boolean, "stop-immediately", "Optional: stop everything NOW, without fadeouts?", false)
			);

			// Sub-commands for Param
			commands[7].add_option(
				dpp::command_option(dpp::co_string, "instance-name", "The name of the event instance to set parameters on.", true)
			);
			commands[7].add_option(
				dpp::command_option(dpp::co_string, "parameter-name", "The name of the parameter to set.", true)
			);
			commands[7].add_option(
				dpp::command_option(dpp::co_number, "value", "What you want the parameter to be.", true)
			);

			// Sub-commands for Volume
			commands[8].add_option(
				dpp::command_option(dpp::co_string, "bus-or-vca-name", "The name of the Bus or VCA to adjust the volume of.", true)
			);
			commands[8].add_option(
				dpp::command_option(dpp::co_number, "value",
					"The target volume in dB. Values above +10 will be assumed negative, for your ears' sake.", true)
			);

			// Permissions. Show commands for only those who can use slash commands in a server.
			// Only the Owner will be allowed to enact commands, but that's checked locally.
			for (int i = 0; i > commands.size(); i++) {
				commands[i].default_member_permissions.has(dpp::permissions::p_use_application_commands);
			}

			bot.global_bulk_command_create(commands);
		}
	});

	/* Handle slash commands */
	bot.on_slashcommand([&bot](const dpp::slashcommand_t& event) {

		// Filter out non-Owners from enacting commands
		//std::cout << "Command received" << std::endl;
		//dpp::snowflake cmdSender = std::get<dpp::snowflake>(event.get_parameter("user"));
		dpp::snowflake cmdSender = event.command.get_issuing_user().id;
		//std::cout << "Command sent by " << event.command.get_issuing_user
		// ().username << " with snowflake " << cmdSender << std::endl;
		bool canRun = false;
		//std::cout << "owningUsers size: " << owningUsers.size() << std::endl;
		for (int i = 0; i < owningUsers.size(); i++) {
			//std::cout << "cmdSender: " << cmdSender.str() << " || owningUser: " << owningUsers[i] << std::endl;
			if (owningUsers[i] == cmdSender) {
				canRun = true;
			}
		}
		if (!canRun) {
			event.reply(dpp::message("Sorry, only the bot owner can run commands for me.").set_flags(dpp::m_ephemeral));
		}
		else {
			if (event.command.get_command_name() == "events") { events(event); }
			else if (event.command.get_command_name() == "list") { list(event); }
			else if (event.command.get_command_name() == "play") { play(event); }
			else if (event.command.get_command_name() == "pause") { pause(event); }
			else if (event.command.get_command_name() == "unpause") { unpause(event); }
			else if (event.command.get_command_name() == "stop") { stop(event); }
			else if (event.command.get_command_name() == "stopall") { stopall(event); }
			else if (event.command.get_command_name() == "param") { param(event); }
			else if (event.command.get_command_name() == "volume") { volume(event); }
			else if (event.command.get_command_name() == "ping") { ping(event); }
			else if (event.command.get_command_name() == "banks") { banks(event); }
			else if (event.command.get_command_name() == "join") { join(event); }
			else if (event.command.get_command_name() == "leave") { leave(event); }
			else if (event.command.get_command_name() == "quit") { quit(event); }
			else {
				std::string enteredname = event.command.get_command_name();
				event.reply(dpp::message("Sorry, " + enteredname + " isn't a command I understand. Apologies.").set_flags(dpp::m_ephemeral));
			}
		}
	});

	/* Set currentClient and tell the program we're connected */
	bot.on_voice_ready([&bot](const dpp::voice_ready_t& event) {
		std::cout << "Voice Ready" << std::endl;
		currentClient = event.voice_client;							// Get the bot's current voice channel
		currentClient->set_send_audio_type(dpp::discord_voice_client::satype_live_audio);
		isConnected = true;											// Tell the rest of the program we've connected
	});

	/* Just confirm when we're leaving Voice. Everything is stopped
	   and null'd before this is called, but it's good to have just in case.
	*/
	bot.on_voice_client_disconnect([&bot](const dpp::voice_client_disconnect_t& event) {
		std::cout << "Voice Disconnecting." << std::endl;
		isConnected = false;
	});

	/* Start the bot */
	bot.start();

	/* Program loop */
	while (!exitRequested) {
#ifndef NDEBUG
		//Update time
		last = end;
		end = std::chrono::system_clock::now();
#endif
		// Send PCM data to D++, if applicable
		if (isConnected) {
#ifndef NDEBUG
			//Start timer the first time we enter "isConnected"
			if (timerNotRunning) {
				start = std::chrono::system_clock::now();
				end = std::chrono::system_clock::now();
				timerNotRunning = false;
			}
			elapsed = end - start;
			elapsedFrame = end - last;
			std::cout << "Frame time: " << elapsedFrame << " || Samples added: " << samplesAddedCounter << " || Samples in buffer: " << myPCMData.size() << std::endl;
			samplesAddedCounter = 0;
#endif
			if (myPCMData.size() > dpp::send_audio_raw_max_length) {								// If buffer is full enough (note: big enough so half of buffer always remains)
#ifndef NDEBUG
				std::cout << "Sending PCM Data at time: " << elapsed << std::endl;
#endif
				while (myPCMData.size() > (dpp::send_audio_raw_max_length * 0.5)) {									// Until minimum size we want our buffer
					currentClient->send_audio_raw((uint16_t*)myPCMData.data(), dpp::send_audio_raw_max_length);		// Send the buffer (method takes 11520 BYTES, so 5760 samples)
					myPCMData.erase(myPCMData.begin(), myPCMData.begin() + (int)(dpp::send_audio_raw_max_length * 0.5));	// Trim our main buffer of the data just sent
				}
			}
#ifndef NDEBUG
			else {																						//Else just report how much is left in the D++ buffer
				std::cout << "D++ Seconds remaining: " << currentClient->get_secs_remaining() << std::endl;
			}
#endif
			// Todo: what if the audio ends? Or if completely silent? We should stop trying to transmit normally, right? Probably would go here.
			// Possible approach: !eventsPlaying && output is silent, fromSilence = true.
		}
		// Update FMOD processes. Just before "Sleep" which gives FMOD some time to process without main thread interference.
		pSystem->update();
		Sleep(20);
	}

	// Quitting program.
	std::cout << "Quitting program. Releasing resources...";

	// Todo: If in voice, leave chat before dying?

	// Remove DSP from master channel group, and release the DSP
	pMasterBusGroup->removeDSP(mCaptureDSP);
	mCaptureDSP->release();

	// Unload and release FMOD Studio System
	pSystem->unloadAll();
	pSystem->release();

	// Todo: Any cleanup necessary for the bot?

	std::cout << std::endl;

	return 0;
}