// The biped as a side-scrolling platformer hero: a side camera, movement
// along the course only, a high Mario-like jump, ? blocks headbutted from
// below, and (with -RigExecAutoplay) an autopilot that plays the course on
// its own, reading it with traces.
#pragma once

#include "CoreMinimal.h"
#include "RigExecBipedCharacter.h"

#include "RigExecPlatformerCharacter.generated.h"

class ARigExecPlatformerLevel;

UCLASS()
class RIGEXECBIPED_API ARigExecPlatformerCharacter : public ARigExecBipedCharacter
{
	GENERATED_BODY()

public:
	ARigExecPlatformerCharacter();

	/** Plays the course by itself: runs right, jumps walls, gaps and ? blocks. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Platformer")
	bool bAutoplay = false;

	ARigExecPlatformerLevel* GetLevel() const { return Level; }
	/** Past the flagpole. */
	bool IsFinished() const { return bFinished; }
	float GetCourseTime() const { return CourseTime; }

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void NotifyHit(UPrimitiveComponent* MyComp, AActor* Other, UPrimitiveComponent* OtherComp, bool bSelfMoved,
	                       FVector HitLocation, FVector HitNormal, FVector NormalImpulse, const FHitResult& Hit) override;

protected:
	virtual void Move(const FInputActionValue& Value) override;

private:
	void Autopilot();
	bool Trace(const FVector& From, const FVector& To, FHitResult& Out) const;

	UPROPERTY(Transient)
	TObjectPtr<ARigExecPlatformerLevel> Level;
	bool bFinished = false;
	bool bAtDoor = false;
	float CourseTime = 0.0f;
	float HoldJumpUntil = 0.0f;
	float NextJump = 0.0f;
	FVector LastSafe = FVector::ZeroVector;
	/** Where the camera looks: ahead of the hero, above the ground stood on. */
	FVector CameraAt = FVector::ZeroVector;
	float CameraLead = 250.0f;
	float CameraHeight = 380.0f;
};
