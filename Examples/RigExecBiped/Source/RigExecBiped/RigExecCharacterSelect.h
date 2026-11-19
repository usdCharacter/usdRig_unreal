// The character lobby: every baked character standing on its own glowing
// pad, idling, in front of a camera. The chosen one steps forward and its pad
// lights up. Built at runtime above the level (the level's sun and sky light
// it), so any game level can open it; the menu HUD drives it.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "RigExecCharacterSelect.generated.h"

class ARigExecBipedCharacter;
class UCameraComponent;
class UMaterialInstanceDynamic;
class UPointLightComponent;
class UStaticMeshComponent;

UCLASS()
class RIGEXECBIPED_API ARigExecCharacterSelect : public AActor
{
	GENERATED_BODY()

public:
	ARigExecCharacterSelect();

	UPROPERTY(VisibleAnywhere, Category = "Select")
	TObjectPtr<UCameraComponent> Camera;

	/** Stages one preview per character asset, the focused one forward. Pad
	 * `Skip` stays empty: the player, already loaded, is lent to it. */
	void Stage(const TArray<FSoftObjectPath>& Characters, int32 Focus, int32 Skip = INDEX_NONE);
	/** Stands a character already loaded (the player) on empty pad `Index`
	 * while the lobby is open, so the lobby never loads a second copy of it.
	 * False when the pad has a preview of its own. */
	bool Lend(int32 Index, ARigExecBipedCharacter* Character);
	/** Gives the lent character back where it stood. */
	void Return();
	/** The player chose someone else: the lent character stays on its pad as
	 * that preview for good. Returns where it stood (the new player goes there). */
	FTransform Keep();
	bool IsLent(const ARigExecBipedCharacter* Character) const;
	/** Hidden between visits, with its previews asleep (not ticking, no rig
	 * running), so reopening the chooser loads nothing. */
	void SetShown(bool bShown);
	void SetFocus(int32 Index);
	/** Hands preview `Index` over (to become the player); the lobby forgets it. */
	ARigExecBipedCharacter* Take(int32 Index);
	int32 GetFocus() const { return Focus; }
	int32 Num() const { return Previews.Num(); }
	/** Where a preview stands (its feet) and how tall it is, for the HUD's labels. */
	bool GetPreviewBounds(int32 Index, FVector& Feet, float& Height) const;

	virtual void Tick(float DeltaSeconds) override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;

private:
	UStaticMeshComponent* AddShape(const TCHAR* Mesh, const FVector& At, const FVector& Scale, const FLinearColor& Color);
	FVector Slot(int32 Index) const;

	UPROPERTY(Transient)
	TArray<TObjectPtr<ARigExecBipedCharacter>> Previews;
	UPROPERTY(Transient)
	TArray<TObjectPtr<UStaticMeshComponent>> Pads;
	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInstanceDynamic>> PadLooks;
	UPROPERTY(Transient)
	TArray<TObjectPtr<UPointLightComponent>> PadLights;
	UPROPERTY(Transient)
	TObjectPtr<UPointLightComponent> Key;
	UPROPERTY(Transient)
	TObjectPtr<UPointLightComponent> Rim;

	int32 Focus = 0;
	float Time = 0.0f;
	/** The pad the player is lent to, and where the player stood. */
	int32 Lent = INDEX_NONE;
	FTransform LentFrom;
	/** How far forward each preview stands now (eases to 1 when focused). */
	TArray<float> Forward;
};
