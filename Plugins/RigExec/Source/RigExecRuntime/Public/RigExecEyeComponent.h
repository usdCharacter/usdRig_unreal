// Draws a RigExec character's eyes with the Squarebit Eye shader.
//
// The rig's surface projectors publish, per eye mesh, the frame the iris
// is projected from ("<eye>.primvars:eyeProjector") and the eye control's
// shader dials packed in a matrix ("<eye>.primvars:eyeDials"). This puts
// the Squarebit Eye material on each eye mesh and, every frame after the
// rig has evaluated, writes the projector into the meshes' Custom Primitive
// Data in the Eye's packed layout (its SPEC section 5) and the dials into
// the material. It needs the SquarebitEye plugin's materials in the
// project, and nothing of it at build time.
#pragma once

#include "Components/ActorComponent.h"
#include "CoreMinimal.h"

#include "RigExecEyeComponent.generated.h"

class UMaterialInstanceDynamic;
class UMaterialInterface;
class UPrimitiveComponent;
class URigExecComponent;

UCLASS(ClassGroup = (Rendering), meta = (BlueprintSpawnableComponent, DisplayName = "RigExec Squarebit Eye"))
class RIGEXECRUNTIME_API URigExecEyeComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	URigExecEyeComponent();

	/** The rig whose eyes to draw; the owner's when empty. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RigExec")
	TObjectPtr<URigExecComponent> Rig;

	/** The Eye's left and right master materials (they read eye 0's and eye
	 * 1's Custom Primitive Data). */
	UPROPERTY(EditAnywhere, Category = "RigExec")
	TSoftObjectPtr<UMaterialInterface> LeftMaterial;
	UPROPERTY(EditAnywhere, Category = "RigExec")
	TSoftObjectPtr<UMaterialInterface> RightMaterial;

	/** The primvars the projectors publish. */
	UPROPERTY(EditAnywhere, Category = "RigExec")
	FString ProjectorPrimvar = TEXT("eyeProjector");
	UPROPERTY(EditAnywhere, Category = "RigExec")
	FString DialPrimvar = TEXT("eyeDials");

	/** The look before the dials: sizes the dials multiply (1 + dial), as the
	 * rig's own shader does. The biped's eye material's values. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RigExec")
	float PupilRadius = 0.1667f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RigExec")
	float IrisRadius = 0.4875f;

	/** Refraction against an implicit sphere (1) rather than the mesh's
	 * normals (0): the Eye's advice for a subdivided or deforming eye. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RigExec")
	int32 RefractionMode = 1;

	/** What it is drawing, or why not. */
	UPROPERTY(VisibleAnywhere, Transient, Category = "RigExec")
	FString Status;

	/** Puts the materials on the eye meshes now (they are found again after
	 * each load of the rig). Call before something gathers the meshes'
	 * materials, such as a subdivision surface. */
	UFUNCTION(BlueprintCallable, Category = "RigExec")
	bool Bind();

	/** Writes the current projectors and dials. Runs every tick; call it
	 * after evaluating the rig yourself to have it in the same frame. */
	UFUNCTION(BlueprintCallable, Category = "RigExec")
	void Update();

	virtual void TickComponent(float DeltaTime, ELevelTick TickType,
	                           FActorComponentTickFunction* ThisTickFunction) override;

private:
	struct FEye
	{
		int32 Mesh = INDEX_NONE;
		/** 0 left, 1 right: which Custom Primitive Data block, which material. */
		int32 Index = 0;
		FString Projector;
		FString Dials;
		TArray<TObjectPtr<UMaterialInstanceDynamic>> Looks;
		/** The dials last sent, so unchanged ones are not sent again. */
		float Sent[8] = {-1e30f, -1e30f, -1e30f, -1e30f, -1e30f, -1e30f, -1e30f, -1e30f};
		/** What each dial scales (or offsets from): the eye's own look values
		 * (its material's IrisWidth, LimbusRadius, ...), else the defaults. */
		float Base[8] = {1, 1, 1, 1, 0, 0, 1, 1};
	};
	TArray<FEye> Eyes;
	/** Keeps the material instances alive. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInstanceDynamic>> Held;
	int32 BoundLoad = -1;
};
