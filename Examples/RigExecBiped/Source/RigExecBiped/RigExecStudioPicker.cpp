#include "RigExecStudioPicker.h"
#include "RigExecVR.h"

#include "CanvasItem.h"
#include "Dom/JsonObject.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "RigExecComponent.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "TextureResource.h"

namespace RigExecStudio
{
namespace
{
// The IK/FK dial is the blend weight, 0 FK and 1 IK; in between both halves drive.
constexpr float GModeEnd = 0.001f;

FColor ReadColor(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, FColor Default)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Object->TryGetArrayField(Field, Values) || Values->Num() < 3)
	{
		return Default;
	}
	auto Channel = [&](int32 I, uint8 Fallback) {
		return Values->IsValidIndex(I) ? uint8(FMath::Clamp((*Values)[I]->AsNumber(), 0.0, 255.0)) : Fallback;
	};
	return FColor(Channel(0, 0), Channel(1, 0), Channel(2, 0), Channel(3, 255));
}

TArray<FVector2f> ReadPoints(const TArray<TSharedPtr<FJsonValue>>& Flat)
{
	TArray<FVector2f> Points;
	for (int32 I = 0; I + 1 < Flat.Num(); I += 2)
	{
		Points.Emplace(float(Flat[I]->AsNumber()), float(Flat[I + 1]->AsNumber()));
	}
	return Points;
}

FLinearColor Tint(FColor Color, float Opacity)
{
	// The picker's colours are sRGB; the canvas takes linear.
	FLinearColor Linear(Color);
	Linear.A *= Opacity;
	return Linear;
}
} // namespace

bool FPicker::Load(URigExecComponent* Rig)
{
	Panels.Reset();
	Loaded = Rig;
	FString Text;
	TSharedPtr<FJsonObject> Root;
	if (!Rig || !Rig->GetPickerText(Text) || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) ||
	    !Root.IsValid())
	{
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>* Pickers = nullptr;
	if (!Root->TryGetArrayField(TEXT("pickers"), Pickers))
	{
		return false;
	}
	for (const TSharedPtr<FJsonValue>& PickerValue : *Pickers)
	{
		for (const TSharedPtr<FJsonValue>& PanelValue : PickerValue->AsObject()->GetArrayField(TEXT("panels")))
		{
			const TSharedPtr<FJsonObject> PanelObject = PanelValue->AsObject();
			FPickerPanel& Panel = Panels.AddDefaulted_GetRef();
			Panel.Label = PanelObject->GetStringField(TEXT("label"));
			const TArray<TSharedPtr<FJsonValue>>& Size = PanelObject->GetArrayField(TEXT("size"));
			Panel.Size = FVector2f(float(Size[0]->AsNumber()), float(Size[1]->AsNumber()));
			for (const TSharedPtr<FJsonValue>& ButtonValue : PanelObject->GetArrayField(TEXT("buttons")))
			{
				const TSharedPtr<FJsonObject> B = ButtonValue->AsObject();
				FPickerButton& Button = Panel.Buttons.AddDefaulted_GetRef();
				B->TryGetStringField(TEXT("id"), Button.Id);
				const TArray<TSharedPtr<FJsonValue>>& Box = B->GetArrayField(TEXT("box"));
				const FVector2f Min(float(Box[0]->AsNumber()), float(Box[1]->AsNumber()));
				Button.Box = FBox2f(Min, Min + FVector2f(float(Box[2]->AsNumber()), float(Box[3]->AsNumber())));
				Button.Vertices = ReadPoints(B->GetArrayField(TEXT("vertices")));
				for (const TSharedPtr<FJsonValue>& I : B->GetArrayField(TEXT("triangles")))
				{
					Button.Triangles.Add(uint32(I->AsNumber()));
				}
				for (const TSharedPtr<FJsonValue>& Outline : B->GetArrayField(TEXT("outlines")))
				{
					TArray<FVector2f> Points = ReadPoints(Outline->AsArray());
					if (Points.Num() > 1)
					{
						Button.Outlines.Add(MoveTemp(Points));
					}
				}
				Button.Fill = ReadColor(B, TEXT("fill"), FColor(128, 128, 128));
				Button.Stroke = ReadColor(B, TEXT("stroke"), FColor(0, 0, 0, 200));
				Button.TextColor = ReadColor(B, TEXT("textColor"), FColor::Black);
				Button.ValueColor = ReadColor(B, TEXT("valueColor"), FColor(235, 235, 235));
				B->TryGetNumberField(TEXT("strokeWidth"), Button.StrokeWidth);
				B->TryGetStringField(TEXT("text"), Button.Text);
				B->TryGetNumberField(TEXT("fontSize"), Button.FontSize);
				FString Align;
				B->TryGetStringField(TEXT("align"), Align);
				Button.Align = Align == TEXT("left") ? -1 : Align == TEXT("right") ? 1 : 0;
				B->TryGetBoolField(TEXT("decoration"), Button.bDecoration);
				B->TryGetStringField(TEXT("mirror"), Button.Mirror);
				B->TryGetStringField(TEXT("command"), Button.Command);
				if (B->TryGetStringField(TEXT("dial"), Button.DialPath))
				{
					B->TryGetStringField(TEXT("mode"), Button.Mode);
				}
				bool bResolved = true;
				const TArray<TSharedPtr<FJsonValue>>* Targets = nullptr;
				if (B->TryGetArrayField(TEXT("targets"), Targets))
				{
					for (const TSharedPtr<FJsonValue>& Target : *Targets)
					{
						const int32 C = Rig->FindControlIndex(Target->AsString());
						bResolved &= C != INDEX_NONE && !Rig->IsControlNull(C);
						if (C != INDEX_NONE && !Rig->IsControlNull(C))
						{
							Button.Targets.AddUnique(C);
						}
					}
				}
				const TSharedPtr<FJsonObject>* Attribute = nullptr;
				if (!Button.Command.IsEmpty())
				{
					// Zeroing works here; showing and hiding gizmos has nothing to show.
					Button.bLive = Button.Command == TEXT("zero_ctrls");
				}
				else if (B->TryGetObjectField(TEXT("attribute"), Attribute))
				{
					Button.ChannelPath = (*Attribute)->GetStringField(TEXT("path"));
					for (const TSharedPtr<FJsonValue>& Label : (*Attribute)->GetArrayField(TEXT("labels")))
					{
						Button.Labels.Add(Label->AsString());
					}
					(*Attribute)->TryGetBoolField(TEXT("invert"), Button.bInvert);
					FString Owner, Attr;
					if (Button.ChannelPath.Split(TEXT("."), &Owner, &Attr) && Attr == TEXT("avars:space"))
					{
						Button.SwitchedControl = Rig->FindControlIndex(Owner);
					}
					Button.bLive = Rig->FindChannelIndex(Button.ChannelPath) != INDEX_NONE;
				}
				else
				{
					Button.bLive = Button.Targets.Num() > 0 && bResolved;
				}
			}
		}
	}
	return Panels.Num() > 0;
}

int32 FPicker::Shown(const FPickerButton& Button) const
{
	if (!Button.bDecoration && !Button.bLive)
	{
		return 0;
	}
	const URigExecComponent* Rig = Loaded.Get();
	if (Rig && !Button.Mode.IsEmpty() && !Button.DialPath.IsEmpty())
	{
		const int32 Dial = Rig->FindChannelIndex(Button.DialPath);
		if (Dial != INDEX_NONE)
		{
			const double V = Rig->GetChannelValue(Dial);
			const TCHAR* Mode = V >= 1.0 - GModeEnd ? TEXT("ik") : V <= GModeEnd ? TEXT("fk") : nullptr;
			if (Mode && Button.Mode != Mode)
			{
				// The idle half: a decoration of it is not drawn at all.
				return Button.bDecoration ? 0 : 2;
			}
		}
	}
	return 1;
}

FString FPicker::ValueLabel(const FPickerButton& Button) const
{
	const URigExecComponent* Rig = Loaded.Get();
	if (!Rig || Button.ChannelPath.IsEmpty() || Button.Labels.Num() == 0)
	{
		return FString();
	}
	int32 Index = FMath::RoundToInt(Rig->GetChannelValue(Rig->FindChannelIndex(Button.ChannelPath)));
	if (Button.bInvert)
	{
		Index = Button.Labels.Num() - 1 - Index;
	}
	return Button.Labels.IsValidIndex(Index) ? Button.Labels[Index] : FString();
}

int32 FPicker::HitAt(int32 Panel, const FVector2f& At) const
{
	if (!Panels.IsValidIndex(Panel))
	{
		return INDEX_NONE;
	}
	const TArray<FPickerButton>& Buttons = Panels[Panel].Buttons;
	for (int32 K = Buttons.Num() - 1; K >= 0; --K)
	{
		const FPickerButton& Button = Buttons[K];
		if (!Button.bDecoration && Button.bLive && Shown(Button) > 0 && Button.Box.IsInsideOrOn(At))
		{
			return K;
		}
	}
	return INDEX_NONE;
}

void FPicker::Draw(UCanvas* Canvas, int32 PanelIndex, const FVector2f& Offset, float Scale, int32 Hovered,
                   const TArray<int32>& Selected, float Opacity) const
{
	if (!Canvas || !Panels.IsValidIndex(PanelIndex))
	{
		return;
	}
	auto Map = [&Offset, Scale](const FVector2f& P) { return FVector2D(Offset + P * Scale); };
	UFont* Font = GEngine ? GEngine->GetSmallFont() : nullptr;
	const TArray<FPickerButton>& Buttons = Panels[PanelIndex].Buttons;
	// Hundreds of buttons: their fills go in one triangle batch and their
	// outlines in one line batch (one draw each, not one per piece), then
	// the labels over them.
	TArray<FCanvasUVTri> Fills;
	struct FLabel
	{
		FCanvasTextItem Item;
	};
	TArray<FCanvasTextItem> Labels;
	struct FSegment
	{
		FVector2D A, B;
		FLinearColor Color;
		float Thickness;
	};
	TArray<FSegment> Segments;
	for (int32 Index = 0; Index < Buttons.Num(); ++Index)
	{
		const FPickerButton& Button = Buttons[Index];
		const int32 Show = Shown(Button);
		if (Show == 0)
		{
			continue;
		}
		const float Alpha = Opacity * (Show == 2 ? 0.35f : 1.0f);
		FColor Fill = Button.Fill;
		if (Index == Hovered && Button.bLive && !Button.bDecoration)
		{
			auto Lift = [](uint8 C) { return uint8(FMath::Min(int32(C) * 5 / 4 + 20, 255)); };
			Fill = FColor(Lift(Fill.R), Lift(Fill.G), Lift(Fill.B), Fill.A);
		}
		// The shape, filled.
		if (Button.Triangles.Num() >= 3)
		{
			TArray<FCanvasUVTri>& Tris = Fills;
			const FLinearColor Color = Tint(Fill, Alpha);
			for (int32 T = 0; T + 2 < Button.Triangles.Num(); T += 3)
			{
				const uint32 A = Button.Triangles[T], B = Button.Triangles[T + 1], C = Button.Triangles[T + 2];
				if (!Button.Vertices.IsValidIndex(A) || !Button.Vertices.IsValidIndex(B) || !Button.Vertices.IsValidIndex(C))
				{
					continue;
				}
				FCanvasUVTri& Tri = Tris.AddDefaulted_GetRef();
				Tri.V0_Pos = Map(Button.Vertices[A]);
				Tri.V1_Pos = Map(Button.Vertices[B]);
				Tri.V2_Pos = Map(Button.Vertices[C]);
				Tri.V0_Color = Tri.V1_Color = Tri.V2_Color = Color;
			}
		}
		// Its outline: white and heavier when every target is selected.
		bool bSelected = Button.Targets.Num() > 0;
		for (const int32 Target : Button.Targets)
		{
			bSelected &= Selected.Contains(Target);
		}
		FLinearColor Stroke = bSelected ? FLinearColor(1.0f, 1.0f, 1.0f, Alpha) : Tint(Button.Stroke, Alpha);
		const float Thickness = bSelected ? 2.0f : FMath::Max(Button.StrokeWidth, 0.5f);
		for (const TArray<FVector2f>& Outline : Button.Outlines)
		{
			for (int32 P = 0; P < Outline.Num(); ++P)
			{
				Segments.Add({Map(Outline[P]), Map(Outline[(P + 1) % Outline.Num()]), Stroke, Thickness});
			}
		}
		// The label, and an attribute's value after it.
		const FString Value = ValueLabel(Button);
		const FString Label = Button.Text.IsEmpty() ? Value : Value.IsEmpty() ? Button.Text : Button.Text + TEXT(" ") + Value;
		if (Label.IsEmpty() || !Font)
		{
			continue;
		}
		float TextW = 0.0f, TextH = 0.0f;
		Canvas->TextSize(Font, Label, TextW, TextH);
		const FVector2f Size = Button.Box.GetSize() * Scale;
		// The font at the button's size, shrunk until it fits.
		float TextScale = FMath::Max(Button.FontSize, 5.0f) * Scale / FMath::Max(float(Font->GetMaxCharHeight()), 1.0f) * 1.25f;
		TextScale = FMath::Min(TextScale, (Size.X - 2.0f) / FMath::Max(TextW, 1.0f));
		if (TextScale * TextH < 4.0f)
		{
			continue;
		}
		const FVector2f Min = Offset + Button.Box.Min * Scale;
		const float W = TextW * TextScale, H = TextH * TextScale;
		const float X = Button.Align < 0 ? Min.X + 2.0f : Button.Align > 0 ? Min.X + Size.X - W - 2.0f : Min.X + (Size.X - W) * 0.5f;
		FCanvasTextItem& Item = Labels.Emplace_GetRef(FVector2D(X, Min.Y + (Size.Y - H) * 0.5f), FText::FromString(Label), Font,
		                                              Tint(Value.IsEmpty() ? Button.TextColor : Button.ValueColor, Alpha));
		Item.Scale = FVector2D(TextScale, TextScale);
	}
	// The canvas draws its batches in the order they are made: the fills,
	// then the outlines over them, then the labels over both.
	(void)sizeof(FLabel);
	if (Fills.Num() > 0)
	{
		FCanvasTriangleItem Item(Fills, GWhiteTexture);
		Item.BlendMode = ARigExecVRRig::UiBlend();
		Canvas->DrawItem(Item);
	}
	if (FBatchedElements* Lines = Canvas->Canvas ? Canvas->Canvas->GetBatchedElements(FCanvas::ET_Line) : nullptr)
	{
		for (const FSegment& Segment : Segments)
		{
			Lines->AddLine(FVector(Segment.A.X, Segment.A.Y, 0.0), FVector(Segment.B.X, Segment.B.Y, 0.0), Segment.Color, FHitProxyId(),
			               Segment.Thickness, 0.0f, true);
		}
	}
	for (FCanvasTextItem& Item : Labels)
	{
		Item.BlendMode = ARigExecVRRig::UiTextBlend();
		Canvas->DrawItem(Item);
	}
}
} // namespace RigExecStudio
