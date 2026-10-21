#include "RigExecPlatformerLevel.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "UObject/ConstructorHelpers.h"

namespace
{
// Where a spent coin or block instance goes: out of sight, out of reach.
const FTransform GHidden(FRotator::ZeroRotator, FVector(0.0, 0.0, -100000.0), FVector(0.01));

const FLinearColor GGround(0.30f, 0.13f, 0.04f);
const FLinearColor GBrick(0.62f, 0.22f, 0.06f);
const FLinearColor GQuestion(1.00f, 0.62f, 0.02f);
const FLinearColor GSpent(0.22f, 0.13f, 0.07f);
const FLinearColor GHard(0.45f, 0.26f, 0.12f);
const FLinearColor GPipe(0.02f, 0.42f, 0.04f);
const FLinearColor GCoin(1.00f, 0.72f, 0.05f);
const FLinearColor GPole(0.55f, 0.60f, 0.55f);
const FLinearColor GFlag(0.05f, 0.65f, 0.10f);
const FLinearColor GCastle(0.62f, 0.30f, 0.20f);
const FLinearColor GDoor(0.02f, 0.02f, 0.02f);
const FLinearColor GHill(0.10f, 0.45f, 0.08f);
const FLinearColor GBush(0.15f, 0.60f, 0.10f);
const FLinearColor GCloud(0.95f, 0.97f, 1.00f);
const FLinearColor GField(0.20f, 0.42f, 0.10f);
} // namespace

ARigExecPlatformerLevel::ARigExecPlatformerLevel()
{
	PrimaryActorTick.bCanEverTick = true;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeMesh(TEXT("/Engine/BasicShapes/Cube.Cube"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CylinderMesh(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> SphereMesh(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> BasicMaterial(
		TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	Cube = CubeMesh.Object;
	Cylinder = CylinderMesh.Object;
	Sphere = SphereMesh.Object;
	Material = BasicMaterial.Object;
	// World 1-1, more or less (Scripts/make_platformer_layout.py writes it).
	Layout = {
		TEXT("                                                                                                                                                      "),
		TEXT("                                                                                                                                                      "),
		TEXT("                                                                                                                                                      "),
		TEXT("                                                                                                                                                      "),
		TEXT("                 ?                                  ??                            B?B                           X                                     "),
		TEXT("                                             o                 o                                               XX                                     "),
		TEXT("               ooooo        oo              o o               o o   oooooo                        oo          XXX                                     "),
		TEXT("                                                  oooooo                                         o  o        XXXX                                     "),
		TEXT("          ?    B?B?B       o  o            o   o  BBBBBB     o   o   X  X       ?B?B?                       XXXXX               CCC                   "),
		TEXT("                                TT                                  XX  XX                      o    o     XXXXXX              CCCCC                  "),
		TEXT("                        TT      PP                                 XXX  XXX             TT                XXXXXXX              CCDCC                  "),
		TEXT("   S                    PP      PP                                XXXX  XXXX            PP               XXXXXXXX         F    CCCCC                  "),
		TEXT("############################################   ###############   #################################   #################################################"),
	};
}

FVector
ARigExecPlatformerLevel::TileCentre(int32 Column, int32 Row) const
{
	// Row 0 is the ground, whose top is at z = 0.
	return FVector(Column * Tile, 0.0, (Row - 0.5) * Tile);
}

UInstancedStaticMeshComponent*
ARigExecPlatformerLevel::MakeSet(const TCHAR* Name, UStaticMesh* Mesh, const FLinearColor& Color, bool bCollide)
{
	UInstancedStaticMeshComponent* Set = NewObject<UInstancedStaticMeshComponent>(this, Name, RF_Transient);
	Set->SetStaticMesh(Mesh);
	Set->SetupAttachment(RootComponent);
	Set->SetMobility(EComponentMobility::Movable);
	Set->SetCollisionEnabled(bCollide ? ECollisionEnabled::QueryAndPhysics : ECollisionEnabled::NoCollision);
	if (bCollide)
	{
		Set->SetCollisionResponseToAllChannels(ECR_Block);
	}
	if (Material)
	{
		UMaterialInstanceDynamic* Look = UMaterialInstanceDynamic::Create(Material, Set);
		Look->SetVectorParameterValue(TEXT("Color"), Color);
		Set->SetMaterial(0, Look);
	}
	Set->RegisterComponent();
	Sets.Add(Set);
	return Set;
}

void
ARigExecPlatformerLevel::Place(UInstancedStaticMeshComponent* Set, const FVector& Location, const FVector& Size,
                               const FRotator& Rotation)
{
	// The basic shapes are 100 cm across.
	Set->AddInstance(FTransform(Rotation, Location, Size / 100.0), /*bWorldSpace*/ false);
}

void
ARigExecPlatformerLevel::Build()
{
	for (UInstancedStaticMeshComponent* Set : Sets)
	{
		if (Set)
		{
			Set->DestroyComponent();
		}
	}
	Sets.Reset();
	QuestionSpent.Reset();
	CoinState.Reset();
	Coins = 0;
	if (!Cube || !Cylinder || !Sphere)
	{
		return;
	}
	UInstancedStaticMeshComponent* Ground = MakeSet(TEXT("Ground"), Cube, GGround, true);
	UInstancedStaticMeshComponent* Bricks = MakeSet(TEXT("Bricks"), Cube, GBrick, true);
	UInstancedStaticMeshComponent* Hard = MakeSet(TEXT("Hard"), Cube, GHard, true);
	UInstancedStaticMeshComponent* Pipes = MakeSet(TEXT("Pipes"), Cube, GPipe, true);
	UInstancedStaticMeshComponent* Castle = MakeSet(TEXT("Castle"), Cube, GCastle, false);
	UInstancedStaticMeshComponent* Door = MakeSet(TEXT("Door"), Cube, GDoor, false);
	UInstancedStaticMeshComponent* Pole = MakeSet(TEXT("Pole"), Cylinder, GPole, false);
	UInstancedStaticMeshComponent* Flag = MakeSet(TEXT("Flag"), Cube, GFlag, false);
	UInstancedStaticMeshComponent* Hills = MakeSet(TEXT("Hills"), Sphere, GHill, false);
	UInstancedStaticMeshComponent* Bushes = MakeSet(TEXT("Bushes"), Sphere, GBush, false);
	UInstancedStaticMeshComponent* Clouds = MakeSet(TEXT("Clouds"), Sphere, GCloud, false);
	Questions = MakeSet(TEXT("Questions"), Cube, GQuestion, true);
	Spent = MakeSet(TEXT("Spent"), Cube, GSpent, true);
	CoinSet = MakeSet(TEXT("Coins"), Cylinder, GCoin, false);
	for (UInstancedStaticMeshComponent* Set : {Ground, Bricks, Hard, Pipes, Questions.Get(), Spent.Get()})
	{
		Set->SetCastShadow(true);
	}

	const int32 Rows = Layout.Num();
	int32 Width = 0;
	const FVector Block(Tile, Depth, Tile);
	for (int32 R = 0; R < Rows; ++R)
	{
		const int32 Row = Rows - 1 - R;
		const FString& Line = Layout[R];
		Width = FMath::Max(Width, Line.Len());
		for (int32 Col = 0; Col < Line.Len(); ++Col)
		{
			const FVector At = TileCentre(Col, Row);
			switch (Line[Col])
			{
			case TEXT('#'):
				// The ground goes six tiles down, so a gap is a pit.
				Place(Ground, At - FVector(0, 0, Tile * 2.5), FVector(Tile, Depth, Tile * 6.0));
				break;
			case TEXT('B'):
				Place(Bricks, At, Block * 0.98);
				break;
			case TEXT('X'):
				Place(Hard, At, Block * 0.98);
				break;
			case TEXT('?'):
				Place(Questions, At, Block * 0.98);
				QuestionSpent.Add(false);
				break;
			case TEXT('P'):
				Place(Pipes, At, FVector(Tile, Depth * 0.6, Tile));
				break;
			case TEXT('T'):
				Place(Pipes, At, FVector(Tile * 1.12, Depth * 0.7, Tile));
				break;
			case TEXT('o'):
				CoinState.Add({At});
				Place(CoinSet, At, FVector(55.0, 55.0, 12.0), FRotator(0.0, 0.0, 90.0));
				break;
			case TEXT('S'):
				Start = At - FVector(0, 0, Tile * 0.5);
				break;
			case TEXT('F'):
			{
				// A pole nine tiles tall on a block, a ball on top, the flag near the top.
				const FVector Base = At - FVector(0, 0, Tile * 0.5);
				GoalX = Base.X;
				Place(Hard, Base + FVector(0, 0, Tile * 0.5), Block * 0.98);
				Place(Pole, Base + FVector(0, 0, Tile * 5.5), FVector(12.0, 12.0, Tile * 9.0));
				Place(Pole, Base + FVector(0, 0, Tile * 10.1), FVector(30.0));
				Place(Flag, Base + FVector(-Tile * 0.45, 0, Tile * 9.3), FVector(Tile * 0.8, 10.0, Tile * 0.6));
				break;
			}
			case TEXT('C'):
				Place(Castle, At, Block * 0.98);
				break;
			case TEXT('D'):
				Place(Door, At + FVector(0, Depth * 0.5, 0), FVector(Tile * 0.7, 10.0, Tile));
				Place(Castle, At - FVector(0, Tile * 0.2, 0), Block * 0.9);
				DoorX = At.X;
				break;
			default:
				break;
			}
		}
	}
	CoinTotal = CoinState.Num();

	// A field behind the course (the camera is at +Y), level with it and out
	// to the horizon, and scenery on it on a fixed rhythm.
	const float Span = Width * Tile;
	UInstancedStaticMeshComponent* Field = MakeSet(TEXT("Field"), Cube, GField, false);
	Place(Field, FVector(Span * 0.5, -400.0 - 100000.0, -1000.0), FVector(Span + 400000.0, 200000.0, 2000.0));
	for (float X = -600.0f; X < Span + 1200.0f; X += 2400.0f)
	{
		Place(Hills, FVector(X, -2200.0, -100.0), FVector(1400.0, 600.0, 700.0));
		Place(Hills, FVector(X + 1000.0, -2600.0, -100.0), FVector(900.0, 500.0, 380.0));
		Place(Bushes, FVector(X + 1600.0, -450.0, 10.0), FVector(260.0, 120.0, 110.0));
		Place(Bushes, FVector(X + 1780.0, -450.0, 10.0), FVector(200.0, 110.0, 90.0));
		Place(Clouds, FVector(X + 500.0, -3200.0, 1300.0), FVector(520.0, 200.0, 200.0));
		Place(Clouds, FVector(X + 700.0, -3200.0, 1380.0), FVector(380.0, 200.0, 180.0));
		Place(Clouds, FVector(X + 1800.0, -3400.0, 1600.0), FVector(600.0, 220.0, 220.0));
	}
}

void
ARigExecPlatformerLevel::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	Build();
}

void
ARigExecPlatformerLevel::BeginPlay()
{
	Super::BeginPlay();
	Build();
}

bool
ARigExecPlatformerLevel::IsLiveQuestion(const UPrimitiveComponent* Component, int32 Instance) const
{
	return Component && Component == Questions && QuestionSpent.IsValidIndex(Instance) && !QuestionSpent[Instance];
}

void
ARigExecPlatformerLevel::HeadHit(const UPrimitiveComponent* Component, int32 Instance)
{
	if (!IsLiveQuestion(Component, Instance))
	{
		return;
	}
	QuestionSpent[Instance] = true;
	FTransform Where;
	Questions->GetInstanceTransform(Instance, Where);
	Questions->UpdateInstanceTransform(Instance, GHidden, false, true, true);
	Spent->AddInstance(Where);
	// Its coin pops out of the top and counts at once.
	FCoin& Coin = CoinState.Add_GetRef({Where.GetLocation() + FVector(0, 0, Tile)});
	Coin.bTaken = true;
	Coin.Popped = Time;
	CoinSet->AddInstance(FTransform(FRotator(0.0, 0.0, 90.0), Coin.Location, FVector(0.55, 0.55, 0.12)));
	++Coins;
	++CoinTotal;
}

void
ARigExecPlatformerLevel::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	Time += DeltaSeconds;
	if (!CoinSet)
	{
		return;
	}
	const ACharacter* Player = UGameplayStatics::GetPlayerCharacter(this, 0);
	const FVector Body = Player ? Player->GetActorLocation() : FVector(1e9);
	// Coins spin; one the player touches is taken; a popped one rises and goes.
	const FQuat Spin = FRotator(0.0, Time * 220.0, 90.0).Quaternion();
	for (int32 I = 0; I < CoinState.Num(); ++I)
	{
		FCoin& Coin = CoinState[I];
		if (!Coin.bTaken && FMath::Abs(Body.X - Coin.Location.X) < 70.0 && FMath::Abs(Body.Z - Coin.Location.Z) < 120.0)
		{
			Coin.bTaken = true;
			++Coins;
		}
		FTransform Now(Spin, Coin.Location, FVector(0.55, 0.55, 0.12));
		if (Coin.Popped >= 0.0f)
		{
			const float Age = Time - Coin.Popped;
			Now.SetLocation(Coin.Location + FVector(0, 0, 250.0 * FMath::Sin(FMath::Min(Age / 0.5f, 1.0f) * PI)));
			if (Age > 0.5f)
			{
				Now = GHidden;
			}
		}
		else if (Coin.bTaken)
		{
			Now = GHidden;
		}
		CoinSet->UpdateInstanceTransform(I, Now, false, I == CoinState.Num() - 1, true);
	}
}
