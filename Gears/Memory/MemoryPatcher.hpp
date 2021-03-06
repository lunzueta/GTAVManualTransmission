#pragma once

namespace MemoryPatcher {
bool PatchInstructions();
bool RestoreInstructions();

bool PatchSteeringCorrection();
bool RestoreSteeringCorrection();

bool PatchSteeringControl();
bool RestoreSteeringControl();

bool PatchBrakeDecrement();
bool RestoreBrakeDecrement();


/*
	* "total" refers to the "package" of patches needed to get the gearbox
	* and clutch stuff working.
	*/
extern int NumGearboxPatches;
extern int TotalPatched;

/*
	* That means SteeringPatched is just for the steering part.
	*/
extern bool SteeringPatched;
extern bool SteerControlPatched;
extern bool BrakeDecrementPatched;
}
