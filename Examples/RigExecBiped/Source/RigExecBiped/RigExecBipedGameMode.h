// Spawns the player as the RigExec biped, with a HUD showing the
// subdivision level (and buttons to change it) and the close-up's bars.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "RigExecMenuHUD.h"

#include "RigExecBipedGameMode.generated.h"

UCLASS()
class RIGEXECBIPED_API ARigExecBipedGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	ARigExecBipedGameMode();
};

UCLASS()
class RIGEXECBIPED_API ARigExecBipedHUD : public ARigExecMenuHUD
{
	GENERATED_BODY()

public:
	virtual void NotifyHitBoxClick(FName BoxName) override;

protected:
	virtual void DrawGame() override;

private:
	float Bars = 0.0f;
};
