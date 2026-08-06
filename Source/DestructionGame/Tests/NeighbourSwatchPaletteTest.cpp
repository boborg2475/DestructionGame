// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialInterface.h"
#include "RequiredContent.h"
#include "World/BrickActor.h"

/*
 * WITH_EDITOR AS WELL AS WITH_DEV_AUTOMATION_TESTS, for the reason Tests/HighlightMaterialPaintTest.cpp
 * states at the same place: a material's node graph lives in editor-only data, so a cooked build has
 * nothing to read. Compiling a reduced version there would leave a test that is green because it
 * stopped asking, which is indistinguishable from one that is green because the palette is right.
 */
#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

/**
 * NAMED NAMESPACE, and named differently from every other one in this module — an anonymous
 * namespace is private to a TRANSLATION UNIT rather than to a file, and a unity build merges many
 * files into one. Every name here carries a NeighbourSwatch prefix so nothing can be ambiguous
 * against HighlightMaterialPaintTestSupport, which reads the same assets for a different claim.
 * See CURRENT_STATE.md.
 */
namespace NeighbourSwatchPaletteTestSupport
{
	/**
	 * HOW FAR APART THE TWO NUMBERS MAY BE, AND IT IS NOT A MEASUREMENT TOLERANCE.
	 *
	 * NOT EXACT ==, DELIBERATELY, AND THE REASON IS THE TWO STORAGE PATHS RATHER THAN ARITHMETIC.
	 * Nothing is computed on either side: one float comes out of a .uasset the editor wrote, the
	 * other out of a decimal literal the compiler parsed, and the two round-trips are not obliged to
	 * land on the same bit pattern for a value a person typed into a colour picker. An exact
	 * comparison would be red about the last bit of a number nobody can see.
	 *
	 * AND IT IS FIVE HUNDRED TIMES SMALLER THAN THE SMALLEST DRIFT THAT COULD EVER MATTER. Two
	 * colours in this palette are picked to be told apart across a wall, so the closest any pair of
	 * them comes is measured in tenths; the drift this file exists to catch — a palette repick that
	 * moved slot 0 from amber to green — is 1.00 in the red channel. 1e-4 linear is also well below
	 * one of the 255 display levels anywhere in the range, so nothing this admits is visible.
	 */
	constexpr float NeighbourSwatchToleranceLinear = 1.0e-4f;

	/** What one material's emissive folded down to, plus where it came from for a message. */
	struct FNeighbourSwatchEmissive
	{
		FLinearColor Value = FLinearColor(0.0f, 0.0f, 0.0f, 0.0f);
		bool bResolved = false;
		FString Source;
	};

	/**
	 * READ ONE MATERIAL'S EMISSIVE CONSTANT, OR REFUSE.
	 *
	 * DELIBERATELY NARROWER THAN HighlightMaterialPaintTest'S EVALUATOR RATHER THAN A SECOND COPY
	 * OF IT. That one folds multiplies, adds, parameters and channel masks because it has to judge
	 * whatever an overlay happens to be authored as. This one is asserting that a C++ literal equals
	 * an asset's colour, and that claim is only meaningful while the asset's colour IS a literal —
	 * the moment an emissive is computed from a parameter or a texture there is no single number for
	 * the swatch to agree with, and the right answer is to say so rather than to fold something and
	 * compare against it. So the one shape it accepts is a plain Constant3Vector straight into
	 * EmissiveColor, and every other shape is reported by name and fails.
	 *
	 * IT FAILS CLOSED IN EVERY ARM. A missing asset, a material instance, missing graph data, an
	 * inline constant on the input, a masked connection or any other node class all come back
	 * unresolved with a message naming what was found — never as black, and never as a plausible
	 * colour that would then be compared and pass.
	 *
	 * THE INLINE-CONSTANT ARM REFUSES RATHER THAN READS, AND THAT IS NOT LAZINESS.
	 * FColorMaterialInput::Constant is an FColor, and converting one to FLinearColor is an sRGB
	 * DECODE — so an inline (0, 204, 0) would read as roughly (0.0, 0.61, 0.0) rather than as the
	 * value divided by 255, and the comparison below would be against a number that means something
	 * else. Refusing is the only reading that cannot be quietly wrong.
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

		/*
		 * A BASE MATERIAL, because the graph is what is being read. A material instance carries
		 * parameter overrides this reader would not see, so accepting one would mean reporting the
		 * parent's colour as the instance's.
		 */
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

		/* GetTracedInput steps through reroute nodes, which carry no value of their own. */
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

		/*
		 * AND THE THREE CHANNELS MUST ARRIVE IN THE ORDER THEY WERE WRITTEN. A component mask on the
		 * connection swizzles them the way HLSL does, so a masked link can hand green's value to red
		 * while the node still reads (0, 0.8, 0) in the editor. The two shapes accepted here are "no
		 * mask at all" and "the whole of RGB selected"; anything else is a permutation or a
		 * replication and is refused rather than read straight.
		 */
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
 * EVERY SWATCH COLOUR IS THE EMISSIVE OF THE MATERIAL ITS OWN SLOT PUTS ON A BRICK.
 *
 * THIS IS THE WHOLE FEATURE, NOT A DETAIL OF IT. A joint row in the piece inspector draws a block
 * of colour and names its far end in words — "course 2 · #4" — and in a wall of 1,220 identical
 * bricks the words are not enough to find a brick by. The colour is the tie. A swatch that draws a
 * different colour from the brick it points at is not a cosmetic fault: it is an answer to "which
 * brick is this row about" that is confidently wrong, which is worse than drawing no swatch at all.
 *
 * WHAT IS BROKEN TODAY, AND WHY 113 GREEN TESTS SAID NOTHING ABOUT IT. The six colours existed
 * TWICE — once as emissive constants in Content/Materials/M_BrickNeighbour0..5, and once as a
 * file-static palette in DestructionGamePlayerController.cpp whose comment asserted in prose that
 * they were "exactly" those emissives. Nothing held the two together, so a palette repick that
 * changed slots 0, 2 and 5 in the assets left the C++ untouched and every test green: the panel
 * drew amber, chartreuse and teal beside bricks lit green, clay and sage. This is the same shape as
 * the break decision needing GetConnectionUtilisation written so that a readout could not become a
 * third hand-copy of it.
 *
 * WHERE THE ONE COPY SHOULD LIVE, AND WHAT IS HONESTLY STILL TWO. The palette has moved beside the
 * paths it must agree with, as DestructionContent::BrickNeighbourSwatchColours, so there is exactly
 * one colour literal in the module and a seventh slot cannot be given a path without a colour
 * without somebody seeing both. THAT IS NOT THE SAME AS THE DRIFT BEING UNSAYABLE, and pretending
 * otherwise would be the more comfortable lie: the asset still holds its own copy, because the
 * shader has to, and a Constant3Vector is editor-only data that no cooked build can read back. The
 * design that WOULD make it unsayable is re-authoring each emissive as a named VectorParameter, at
 * which point the swatch reads GetVectorParameterValue at runtime and the C++ array is deleted.
 * This test is written so that it survives that change unaltered — it would simply become green by
 * construction rather than by assertion, which is the right end state for it.
 *
 * A SWEEP OVER THE SLOTS RATHER THAN SIX NAMED CHECKS, AND THE COUNT IS PART OF THE SWEEP. Six is
 * the model's number — a brick inside a running bond has six joints — but it is not a number this
 * test may assume, because the failure worth catching is a SEVENTH slot arriving with a material and
 * no colour, or with a colour and no EBrickHighlight state to carry it. So the two tables are held
 * to the same length, and every slot in them is required to map to a highlight state that is not
 * None. Adding a slot is adding a row in two adjacent arrays; forgetting either half is red here.
 *
 * THE OTHER HALF OF THE TIE IS ALREADY HELD ELSEWHERE, AND THIS IS DELIBERATELY NOT A SECOND COPY
 * OF IT. World.Brick.HighlightWearsAMaterial pins that a brick in state Neighbour i wears the asset
 * at BrickNeighbourMaterialPaths[i]; Presenter.PieceMenuJointColourSlots pins that joint row i takes
 * colour slot i. What was missing, and is asserted here, is the one link between them: that slot i's
 * SWATCH is the colour slot i's MATERIAL paints. The three compose to the feature.
 *
 * EQUALITY IN THE DATA IS NOT EQUALITY ON SCREEN, AND NOBODY MAY "FIX" A SWATCH TO MATCH A
 * SCREENSHOT. The swatch is a Slate fill drawn at full strength over the panel's near-black
 * background. The brick is an ADDITIVE overlay composited over a lit surface — measured, see
 * HighlightMaterialPaintTest — so the same linear triple leaves the display at one value in the
 * panel and a brighter, washed one on the brick. The two are meant to be the same DECISION, and
 * matching them by eye would introduce a third number rather than removing one. Whether they LOOK
 * alike is a rendering question for a human at a running game; it is not what this asserts.
 *
 * FAILS CLOSED ON ANYTHING IT CANNOT READ. A material that is missing, is an instance, has no
 * graph, or drives its emissive from anything but a plain constant is reported by name and fails,
 * rather than folding to black and being compared. A NaN in the palette fails the finiteness row
 * BEFORE the comparison, because FMath::Abs of a NaN is a NaN and every comparison against it is
 * false — which would read as "within tolerance" if the rows were the other way round.
 *
 * NEEDS A TICKING WORLD: no, and no world at all. It loads six assets, reads one node from each and
 * compares six triples. It renders no pixel and never could — every run is -nullrhi.
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

	/*
	 * ONE COLOUR PER MATERIAL, ASSERTED BEFORE ANYTHING IS COMPARED. A seventh material added
	 * without a seventh colour would otherwise be silently skipped by a loop over the shorter of
	 * the two, and the row it belongs to would draw the transparent no-swatch entry — an absence
	 * that looks exactly like a joint row past the end of the palette.
	 */
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

		/*
		 * THE SLOT HAS TO REACH A BRICK AT ALL. BrickHighlightForNeighbourSlot is the one mapping
		 * from a colour slot to the state a brick wears, and it fails closed to None for anything
		 * past the end of the enum — so a seventh colour and a seventh material with no seventh
		 * EBrickHighlight enumerator would agree perfectly here and light nothing in the world.
		 */
		TestTrue(
			*FString::Printf(
				TEXT("slot %d must map to a brick highlight state; BrickHighlightForNeighbourSlot answers None, so no brick can ever wear this colour"),
				Slot),
			BrickHighlightForNeighbourSlot(Slot) != EBrickHighlight::None);

		/* Fail closed on a degenerate palette entry rather than letting a NaN compare as equal. */
		TestTrue(
			*FString::Printf(
				TEXT("slot %d's swatch must be finite in every channel; it is (%f, %f, %f, %f)"),
				Slot, Swatch.R, Swatch.G, Swatch.B, Swatch.A),
			NeighbourSwatchIsFinite(Swatch));

		/*
		 * AND IT MUST BE OPAQUE, WHICH IS A DIFFERENT CLAIM FROM HAVING THE RIGHT HUE. The panel
		 * paints a row past the end of the palette in a fully transparent colour, on purpose — an
		 * absent swatch is an absence. A palette entry that arrived at zero alpha would draw exactly
		 * that, so the row would look like a row with no neighbour while a brick out in the wall was
		 * lit for it.
		 */
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

		/*
		 * THE DIRECT STATEMENT OF THE BUG. Slot i's swatch and slot i's material are one decision
		 * expressed twice, and this is the only thing in the suite that says so.
		 */
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
