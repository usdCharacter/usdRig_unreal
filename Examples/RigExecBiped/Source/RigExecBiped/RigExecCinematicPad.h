// A glowing circle on the floor: step on it and the biped's close-up
// cinematic starts (ARigExecBipedCharacter::StartCinematic). It arms again
// once the character has stepped off.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "RigExecCinematicPad.generated.h"

class UBoxComponent;
class UMaterialInstanceDynamic;
class UMaterialInterface;
class UPointLightComponent;
class UStaticMeshComponent;

UCLASS()
class RIGEXECBIPED_API ARigExecCinematicPad : public AActor
{
	GENERATED_BODY()

public:
	ARigExecCinematicPad();

	/** The disc: a plane drawn with the glow material (a ring and a soft fill). */
	UPROPERTY(VisibleAnywhere, Category = "Pad")
	TObjectPtr<UStaticMeshComponent> Disc;

	UPROPERTY(VisibleAnywhere, Category = "Pad")
	TObjectPtr<UPointLightComponent> Light;

	/** Where the character has to stand. */
	UPROPERTY(VisibleAnywhere, Category = "Pad")
	TObjectPtr<UBoxComponent> Trigger;

	/** The additive glow material setup_game.py makes (Color, Glow parameters). */
	UPROPERTY(EditAnywhere, Category = "Pad")
	TSoftObjectPtr<UMaterialInterface> GlowMaterial;

	UPROPERTY(EditAnywhere, Category = "Pad")
	FLinearColor Color = FLinearColor(0.25f, 0.75f, 1.0f);

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;

private:
	UFUNCTION()
	void Entered(UPrimitiveComponent* Overlapped, AActor* Other, UPrimitiveComponent* OtherComp, int32 BodyIndex,
	             bool bFromSweep, const FHitResult& Sweep);
	UFUNCTION()
	void Left(UPrimitiveComponent* Overlapped, AActor* Other, UPrimitiveComponent* OtherComp, int32 BodyIndex);

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> Look;
	bool bArmed = true;
	float Time = 0.0f;
};
