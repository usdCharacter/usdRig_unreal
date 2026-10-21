// A side-scrolling platformer course built from an ASCII layout: ground,
// bricks, ? blocks that pay out a coin when headbutted, pipes, stairs,
// coins, a flagpole and a castle, with hills and clouds behind. Everything
// is instanced cubes, cylinders and spheres in flat colours.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "RigExecPlatformerLevel.generated.h"

class UInstancedStaticMeshComponent;
class UMaterialInterface;
class UStaticMesh;

UCLASS()
class RIGEXECBIPED_API ARigExecPlatformerLevel : public AActor
{
	GENERATED_BODY()

public:
	ARigExecPlatformerLevel();

	/**
	 * The course, top row first; each character is one tile:
	 *   #  ground          B  brick          ?  ? block (a coin when hit from below)
	 *   X  hard block      T  pipe top       P  pipe body
	 *   o  coin            S  start          F  flagpole (stands on the tile below)
	 *   C  castle          D  castle door
	 */
	UPROPERTY(EditAnywhere, Category = "Platformer")
	TArray<FString> Layout;

	/** A tile's width and height, and the course's depth, in centimetres. */
	UPROPERTY(EditAnywhere, Category = "Platformer")
	float Tile = 100.0f;

	UPROPERTY(EditAnywhere, Category = "Platformer")
	float Depth = 300.0f;

	/** Where the player starts (the S tile's floor). */
	FVector GetStart() const { return Start; }
	/** The flagpole's and the castle door's X. */
	float GetGoalX() const { return GoalX; }
	float GetDoorX() const { return DoorX; }
	int32 GetCoins() const { return Coins; }
	int32 GetCoinTotal() const { return CoinTotal; }

	/** The player's head hit a block from below: a ? pays out its coin and goes dark. */
	void HeadHit(const UPrimitiveComponent* Component, int32 Instance);
	/** Whether that is a ? block not yet hit (for the autopilot). */
	bool IsLiveQuestion(const UPrimitiveComponent* Component, int32 Instance) const;

	virtual void OnConstruction(const FTransform& Transform) override;
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;

private:
	void Build();
	UInstancedStaticMeshComponent* MakeSet(const TCHAR* Name, UStaticMesh* Mesh, const FLinearColor& Color, bool bCollide);
	void Place(UInstancedStaticMeshComponent* Set, const FVector& Location, const FVector& Size, const FRotator& Rotation = FRotator::ZeroRotator);
	FVector TileCentre(int32 Column, int32 Row) const;

	UPROPERTY()
	TObjectPtr<UStaticMesh> Cube;
	UPROPERTY()
	TObjectPtr<UStaticMesh> Cylinder;
	UPROPERTY()
	TObjectPtr<UStaticMesh> Sphere;
	UPROPERTY()
	TObjectPtr<UMaterialInterface> Material;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UInstancedStaticMeshComponent>> Sets;
	UPROPERTY(Transient)
	TObjectPtr<UInstancedStaticMeshComponent> Questions;
	UPROPERTY(Transient)
	TObjectPtr<UInstancedStaticMeshComponent> Spent;
	UPROPERTY(Transient)
	TObjectPtr<UInstancedStaticMeshComponent> CoinSet;

	TArray<bool> QuestionSpent;
	struct FCoin
	{
		FVector Location;
		bool bTaken = false;
		/** A coin popping out of a ? block: when it started, or < 0. */
		float Popped = -1.0f;
	};
	TArray<FCoin> CoinState;
	FVector Start = FVector::ZeroVector;
	float GoalX = 0.0f;
	float DoorX = 0.0f;
	int32 Coins = 0;
	int32 CoinTotal = 0;
	float Time = 0.0f;
};
