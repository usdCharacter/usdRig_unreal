// Selecting a control of a limb's idle IK/FK half -- an FK control while the
// limb is in IK, the IK control, pole or foot roll while it is in FK --
// switches the limb to that half, matched, as usdview's
// _SwitchPresentedLimbs does: the control then drives from where it is drawn.
// One undo entry per switch. URigExecComponent::bShowIdleLimbHalf keeps the
// idle half drawn (dimmed) so it can be picked in the viewport.
#pragma once

#include "CoreMinimal.h"

class FRigExecAutoMatch
{
public:
	static void Startup();
	static void Shutdown();
};
