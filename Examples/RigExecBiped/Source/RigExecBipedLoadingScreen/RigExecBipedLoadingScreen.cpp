// The loading screen: Content/Splash/LoadingScreen.png filling the window,
// with a pulsing "LOADING" and a throbber in the corner. The movie player
// draws it on its own thread, so it keeps moving while the game thread
// loads the level and the biped.
#include "Brushes/SlateDynamicImageBrush.h"
#include "Framework/Application/SlateApplication.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Misc/FileHelper.h"
#include "Rendering/SlateRenderer.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "MoviePlayer.h"
#include "Styling/CoreStyle.h"
#include "Styling/SlateBrush.h"
#include "UObject/UObjectGlobals.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Images/SThrobber.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScaleBox.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"

namespace
{
const FLinearColor GCyan(0.35f, 0.8f, 1.0f);

class SRigExecLoadingScreen : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SRigExecLoadingScreen) {}
	SLATE_END_ARGS()

	void Construct(const FArguments&)
	{
		Brush = LoadPicture(FPaths::ProjectContentDir() / TEXT("Splash/LoadingScreen.png"));
		FSlateFontInfo Font = FCoreStyle::GetDefaultFontStyle("Bold", 22);
		Font.LetterSpacing = 600;
		ChildSlot
		[
			SNew(SOverlay)
			+ SOverlay::Slot()
			[
				SNew(SImage).Image(FCoreStyle::Get().GetBrush("BlackBrush"))
			]
			+ SOverlay::Slot()
			[
				SNew(SScaleBox)
				.Stretch(EStretch::ScaleToFill)
				[
					SNew(SImage).Image(Brush.IsValid() ? Brush.Get() : FCoreStyle::Get().GetBrush("BlackBrush"))
				]
			]
			+ SOverlay::Slot()
			.HAlign(HAlign_Left)
			.VAlign(VAlign_Bottom)
			.Padding(FMargin(72.0f, 0.0f, 0.0f, 64.0f))
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					SNew(SCircularThrobber)
					.Radius(14.0f)
					.NumPieces(8)
					.Period(0.9f)
					.ColorAndOpacity(GCyan)
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(FMargin(20.0f, 0.0f, 0.0f, 0.0f))
				[
					SNew(STextBlock)
					.Text(NSLOCTEXT("RigExecBiped", "Loading", "LOADING"))
					.Font(Font)
					.ColorAndOpacity(this, &SRigExecLoadingScreen::Pulse)
				]
			]
		];
	}

private:
	/** The picture, decoded here and handed to the renderer as pixels: it
	 * is a loose file, not an asset, and nothing else is loaded yet. */
	static TSharedPtr<FSlateBrush> LoadPicture(const FString& File)
	{
		TArray<uint8> Png;
		if (!FSlateApplication::IsInitialized() || !FFileHelper::LoadFileToArray(Png, *File))
		{
			return nullptr;
		}
		IImageWrapperModule& Wrappers = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
		TSharedPtr<IImageWrapper> Image = Wrappers.CreateImageWrapper(EImageFormat::PNG);
		TArray<uint8> Bgra;
		if (!Image.IsValid() || !Image->SetCompressed(Png.GetData(), Png.Num()) || !Image->GetRaw(ERGBFormat::BGRA, 8, Bgra))
		{
			return nullptr;
		}
		const FName Name(TEXT("RigExecBipedLoadingScreen"));
		const int32 W = int32(Image->GetWidth()), H = int32(Image->GetHeight());
		if (!FSlateApplication::Get().GetRenderer()->GenerateDynamicImageResource(Name, W, H, Bgra))
		{
			return nullptr;
		}
		return MakeShared<FSlateDynamicImageBrush>(Name, FVector2D(W, H));
	}

	FSlateColor Pulse() const
	{
		const double T = FPlatformTime::Seconds();
		FLinearColor Color = GCyan;
		Color.A = 0.55f + 0.45f * float(0.5 + 0.5 * FMath::Sin(T * 3.0));
		return Color;
	}

	TSharedPtr<FSlateBrush> Brush;
};
} // namespace

class FRigExecBipedLoadingScreenModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		if (!IsMoviePlayerEnabled())
		{
			return;
		}
		// The startup screen, built when the movie player is about to start
		// (Slate's renderer is up by then, for the picture), then the same
		// for any level opened later.
		Prepare = GetMoviePlayer()->OnPrepareLoadingScreen().AddStatic(&FRigExecBipedLoadingScreenModule::Show);
		FCoreUObjectDelegates::PreLoadMap.AddRaw(this, &FRigExecBipedLoadingScreenModule::OnPreLoadMap);
	}

	virtual void ShutdownModule() override
	{
		FCoreUObjectDelegates::PreLoadMap.RemoveAll(this);
		if (IsMoviePlayerEnabled())
		{
			GetMoviePlayer()->OnPrepareLoadingScreen().Remove(Prepare);
		}
	}

	virtual bool IsGameModule() const override
	{
		return true;
	}

private:
	FDelegateHandle Prepare;

	void OnPreLoadMap(const FString&)
	{
		if (!GIsEditor)
		{
			Show();
		}
	}

	static void Show()
	{
		FLoadingScreenAttributes Screen;
		Screen.bAutoCompleteWhenLoadingCompletes = true;
		// Long enough to see, even on a fast load.
		Screen.MinimumLoadingScreenDisplayTime = 2.5f;
		Screen.WidgetLoadingScreen = SNew(SRigExecLoadingScreen);
		GetMoviePlayer()->SetupLoadingScreen(Screen);
	}
};

IMPLEMENT_GAME_MODULE(FRigExecBipedLoadingScreenModule, RigExecBipedLoadingScreen);
