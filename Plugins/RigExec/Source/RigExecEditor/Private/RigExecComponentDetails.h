// The Rig component's Details: a banner at the top when its meshes draw on the
// CPU instead of the GPU, saying why in a sentence, and a summary of its
// stacks with a button to the Stack panel. A character asset's Details show
// the Stack panel itself.
#pragma once

#include "CoreMinimal.h"
#include "IDetailCustomization.h"

class FRigExecComponentDetails : public IDetailCustomization
{
public:
	static TSharedRef<IDetailCustomization> MakeInstance() { return MakeShared<FRigExecComponentDetails>(); }
	virtual void CustomizeDetails(IDetailLayoutBuilder& DetailBuilder) override;

	/** Registers the layouts (this and the character asset's) and the
	 * CPU-fallback toast; at module startup. */
	static void Register();
	static void Unregister();
};

class FRigExecCharacterDetails : public IDetailCustomization
{
public:
	static TSharedRef<IDetailCustomization> MakeInstance() { return MakeShared<FRigExecCharacterDetails>(); }
	virtual void CustomizeDetails(IDetailLayoutBuilder& DetailBuilder) override;
};
