// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialInterface.h"
#include "RequiredContent.h"
#include "World/BrickActor.h"

// Editor-only: the material graph is editor-only data, so a cooked build has nothing to read.
#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

/**
 * Named namespace for unity builds. Names carry a NeighbourSwatch prefix to avoid clashing with
 * HighlightMaterialPaintTestSupport, which reads the same assets.
 */
namespace NeighbourSwatchPaletteTestSupport
{
	/**
	 * Allowed difference, linear. Not exact ==: the .uasset float and the parsed C++ literal need
	 * not round-trip to the same bits. Far below one display level and far below any real repick
	 * (e.g. amber to green is 1.0 in red).
	 */
	constexpr float NeighbourSwatchToleranceLinear = 1.0e-4f;

	/** One material's emissive colour, and a description of its source for messages. */
	struct FNeighbourSwatchEmissive
	{
		FLinearColor Value = FLinearColor(0.0f, 0.0f, 0.0f, 0.0f);
		bool bResolved = false;
		FString Source;
	};

	/**
	 * Read a material's emissive, accepting only a plain Constant3Vector wired to EmissiveColor.
	 * Deliberately narrower than HighlightMaterialPaintTest's evaluator: a computed emissive has no
	 * single value to compare. Fails closed (unresolved, with the reason) on a missing asset, an
	 * instance, no graph data, a masked link, any other node, or an inline constant. The inline
	 * constant is an 8-bit FColor whose conversion is an sRGB decode, so it is not comparable.
	 */
	FNeighbourSwatchEmissive NeighbourSwatchEmissiveOf(const TCHAR* Path)
	{
		FNeighbourSwatchEmissive Emissive;

		UMaterialInterface* const Loaded = LoadObject<UMaterialInterface>(nullptr, Path);

		if (Loaded == nullptr)
		{
			Emissive.Source = TEXT("nothing at all — the path does not resolve");
			return Emissive;
		}

		// Base material only: an instance's parameter overrides would be invisible here.
		const UMaterial* const Material = Cast<UMaterial>(Loaded);

		if (Material == nullptr)
		{
			Emissive.Source = FString::Printf(
				TEXT("a %s rather than a base UMaterial, whose graph this test cannot read"),
				*Loaded->GetClass()->GetName());

			return Emissive;
		}

		const UMaterialEditorOnlyData* const Graph = Material->GetEditorOnlyData();

		if (Graph == nullptr)
		{
			Emissive.Source = TEXT("a material carrying no editor-only graph data");
			return Emissive;
		}

		if (Graph->EmissiveColor.UseConstant)
		{
			Emissive.Source = TEXT("an INLINE constant on the input, which is an 8-bit sRGB value and not comparable with a linear literal");
			return Emissive;
		}

		// GetTracedInput steps through reroute nodes.
		const FExpressionInput Traced = Graph->EmissiveColor.GetTracedInput();

		const UMaterialExpressionConstant3Vector* const Constant =
			Cast<UMaterialExpressionConstant3Vector>(Traced.Expression);

		if (Constant == nullptr)
		{
			Emissive.Source = Traced.Expression != nullptr
				? FString::Printf(
					TEXT("a %s, and only a plain Constant3Vector is a colour a C++ literal can be held equal to"),
					*Traced.Expression->GetClass()->GetName())
				: FString(TEXT("NOTHING CONNECTED, so the engine's (0,0,0) default for EmissiveColor"));

			return Emissive;
		}

		// A component mask can swizzle channels; accept only no mask or exactly RGB.
		const bool bStraightThrough = Traced.Mask == 0
			|| (Traced.MaskR != 0 && Traced.MaskG != 0 && Traced.MaskB != 0 && Traced.MaskA == 0);

		if (!bStraightThrough)
		{
			Emissive.Source = FString::Printf(
				TEXT("a Constant3Vector behind a component mask (R%d G%d B%d A%d), which swizzles the channels"),
				Traced.MaskR, Traced.MaskG, Traced.MaskB, Traced.MaskA);

			return Emissive;
		}

		Emissive.Value = FLinearColor(Constant->Constant.R, Constant->Constant.G, Constant->Constant.B, 1.0f);
		Emissive.bResolved = true;
		Emissive.Source = TEXT("a Constant3Vector on EmissiveColor");

		return Emissive;
	}

	float NeighbourSwatchLargestChannelDifference(const FLinearColor& A, const FLinearColor& B)
	{
		return FMath::Max3(
			FMath::Abs(A.R - B.R),
			FMath::Abs(A.G - B.G),
			FMath::Abs(A.B - B.B));
	}

	bool NeighbourSwatchIsFinite(const FLinearColor& Colour)
	{
		return FMath::IsFinite(Colour.R) && FMath::IsFinite(Colour.G)
			&& FMath::IsFinite(Colour.B) && FMath::IsFinite(Colour.A);
	}

	FString NeighbourSwatchDescribe(const FLinearColor& Colour)
	{
		return FString::Printf(TEXT("(%.4f, %.4f, %.4f)"), Colour.R, Colour.G, Colour.B);
	}
}

/**
 * Each swatch colour equals the emissive of the material its slot puts on a brick. The swatch in
 * a joint row is how a player finds the neighbour brick, so a mismatch points at the wrong brick.
 *
 * The colours were once duplicated in the player controller and drifted from the assets unnoticed.
 * The palette now lives beside the paths as DestructionContent::BrickNeighbourSwatchColours, but
 * the asset still holds its own copy. Re-authoring each emissive as a VectorParameter would remove
 * the duplicate; this test would survive that unchanged.
 *
 * A sweep, not six named checks: the tables must be the same length and every slot must map to a
 * non-None EBrickHighlight, so a seventh slot missing either half fails.
 *
 * Complements World.Brick.HighlightWearsAMaterial (state i wears material i) and
 * Presenter.PieceMenuJointColourSlots (row i uses slot i).
 *
 * Equal data is not equal pixels: the swatch is a flat fill, the brick an additive overlay. Do not
 * match them by eye. Fails closed on anything unreadable; the finiteness check runs before the
 * comparison because a NaN difference would compare as within tolerance.
 *
 * No world; runs under -nullrhi and renders nothing.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNeighbourSwatchPaletteTest,
	"DestructionGame.Content.NeighbourSwatchesMatchTheirMaterials",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FNeighbourSwatchPaletteTest::RunTest(const FString& Parameters)
{
	using namespace NeighbourSwatchPaletteTestSupport;

	const TArrayView<const TCHAR* const> Paths(DestructionContent::BrickNeighbourMaterialPaths);
	const TArrayView<const FLinearColor> Palette(DestructionContent::BrickNeighbourSwatchColours);

	// One colour per material; otherwise the loop below silently skips the extra slot.
	TestEqual(
		*FString::Printf(
			TEXT("the swatch palette must carry one colour per neighbour material: there are %d materials and %d colours"),
			Paths.Num(), Palette.Num()),
		Palette.Num(), Paths.Num());

	const int32 SlotCount = FMath::Min(Paths.Num(), Palette.Num());

	for (int32 Slot = 0; Slot < SlotCount; ++Slot)
	{
		const FLinearColor Swatch = Palette[Slot];
		const FNeighbourSwatchEmissive Emissive = NeighbourSwatchEmissiveOf(Paths[Slot]);

		AddInfo(FString::Printf(
			TEXT("slot %d: swatch %s; '%s' emissive %s from %s"),
			Slot, *NeighbourSwatchDescribe(Swatch), Paths[Slot],
			*NeighbourSwatchDescribe(Emissive.Value), *Emissive.Source));

		// The slot must map to a highlight state; None means no brick can wear it.
		TestTrue(
			*FString::Printf(
				TEXT("slot %d must map to a brick highlight state; BrickHighlightForNeighbourSlot answers None, so no brick can ever wear this colour"),
				Slot),
			BrickHighlightForNeighbourSlot(Slot) != EBrickHighlight::None);

		// Fail closed: a NaN would otherwise compare as within tolerance.
		TestTrue(
			*FString::Printf(
				TEXT("slot %d's swatch must be finite in every channel; it is (%f, %f, %f, %f)"),
				Slot, Swatch.R, Swatch.G, Swatch.B, Swatch.A),
			NeighbourSwatchIsFinite(Swatch));

		// Must be opaque: transparent is how the panel draws "no swatch".
		TestEqual(
			*FString::Printf(
				TEXT("slot %d's swatch must be opaque or it draws as no swatch at all; its alpha is %.4f"),
				Slot, Swatch.A),
			Swatch.A, 1.0f);

		TestTrue(
			*FString::Printf(
				TEXT("slot %d: '%s' must fold to a plain colour for its swatch to be held equal to it; it is %s"),
				Slot, Paths[Slot], *Emissive.Source),
			Emissive.bResolved);

		if (!Emissive.bResolved || !NeighbourSwatchIsFinite(Swatch))
		{
			continue;
		}

		// Slot i's swatch must equal slot i's material emissive.
		const float Difference = NeighbourSwatchLargestChannelDifference(Swatch, Emissive.Value);

		TestTrue(
			*FString::Printf(
				TEXT("slot %d: the swatch must be the colour the brick is lit in — the panel paints %s and '%s' paints %s, differing by %.4f in the widest channel where %.4f is the most that is allowed. Fix the one that is stale; do NOT match them by eye, the swatch is a flat fill and the brick is an additive overlay, so equal numbers are not equal pixels"),
				Slot,
				*NeighbourSwatchDescribe(Swatch), Paths[Slot],
				*NeighbourSwatchDescribe(Emissive.Value),
				Difference, NeighbourSwatchToleranceLinear),
			Difference <= NeighbourSwatchToleranceLinear);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
