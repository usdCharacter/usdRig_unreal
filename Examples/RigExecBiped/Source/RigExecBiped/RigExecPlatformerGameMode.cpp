#include "RigExecPlatformerGameMode.h"

#include "CanvasItem.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "RigExecPlatformerCharacter.h"
#include "RigExecPlatformerLevel.h"

ARigExecPlatformerGameMode::ARigExecPlatformerGameMode()
{
	DefaultPawnClass = ARigExecPlatformerCharacter::StaticClass();
	HUDClass = ARigExecPlatformerHUD::StaticClass();
}

void
ARigExecPlatformerHUD::DrawGame()
{
	const ARigExecPlatformerCharacter* Hero = Cast<ARigExecPlatformerCharacter>(GetOwningPawn());
	const ARigExecPlatformerLevel* Level = Hero ? Hero->GetLevel() : nullptr;
	if (!Canvas || !Level)
	{
		return;
	}
	const float W = Canvas->ClipX, H = Canvas->ClipY, S = H / 720.0f;
	const FLinearColor White(1, 1, 1);
	const FLinearColor Gold(1.0f, 0.8f, 0.2f);
	Text(TEXT("BIPED"), W * 0.06f, H * 0.04f, 1.6f * S, White);
	Text(FString::Printf(TEXT("RIGEXEC")), W * 0.06f, H * 0.04f + 34 * S, 1.2f * S, FLinearColor(0.8f, 0.9f, 1.0f));
	Text(FString::Printf(TEXT("COINS x %02d"), Level->GetCoins()), W * 0.33f, H * 0.04f + 16 * S, 1.6f * S, Gold);
	Text(TEXT("WORLD"), W * 0.58f, H * 0.04f, 1.6f * S, White);
	Text(TEXT("1-1"), W * 0.58f, H * 0.04f + 34 * S, 1.4f * S, White);
	Text(TEXT("TIME"), W * 0.82f, H * 0.04f, 1.6f * S, White);
	Text(FString::Printf(TEXT("%03d"), FMath::Max(0, 400 - FMath::FloorToInt(Hero->GetCourseTime() * 2.5f))), W * 0.82f,
	     H * 0.04f + 34 * S, 1.4f * S, White);
	if (Hero->IsFinished())
	{
		Text(TEXT("COURSE CLEAR!"), W * 0.5f, H * 0.42f, 3.0f * S, Gold, true);
		Text(FString::Printf(TEXT("%d of %d coins"), Level->GetCoins(), Level->GetCoinTotal()), W * 0.5f, H * 0.52f,
		     1.5f * S, White, true);
	}
}
