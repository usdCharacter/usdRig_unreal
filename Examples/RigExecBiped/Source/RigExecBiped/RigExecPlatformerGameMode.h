// The platformer: the biped as the hero, and a HUD with coins and time.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "RigExecMenuHUD.h"

#include "RigExecPlatformerGameMode.generated.h"

UCLASS()
class RIGEXECBIPED_API ARigExecPlatformerGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	ARigExecPlatformerGameMode();
};

UCLASS()
class RIGEXECBIPED_API ARigExecPlatformerHUD : public ARigExecMenuHUD
{
	GENERATED_BODY()

protected:
	virtual void DrawGame() override;
};
