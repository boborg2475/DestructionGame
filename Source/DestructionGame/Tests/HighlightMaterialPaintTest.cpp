// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Engine/EngineTypes.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionConstant2Vector.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionConstant4Vector.h"
#include "Materials/MaterialExpressionAdd.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialInterface.h"
#include "RequiredContent.h"

/*
 * WITH_EDITOR AS WELL AS WITH_DEV_AUTOMATION_TESTS, and the whole test rather than half of it.
 * A material's node graph lives in editor-only data, so a cooked build has nothing to read;
 * compiling a reduced version there would leave a test that is green because it stopped asking,
 * which is indistinguishable from one that is green because the assets are right.
 */
#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

/**
 * NAMED NAMESPACE, and named differently from every other one in this module — an anonymous
 * namespace is private to a TRANSLATION UNIT rather than to a file, and a unity build merges
 * many files into one. See CURRENT_STATE.md.
 */
namespace HighlightMaterialPaintTestSupport
{
	/**
	 * WHAT AN UNCONNECTED INPUT COMPILES TO, TAKEN FROM THE ENGINE'S OWN TABLE AND WRITTEN
	 * DOWN HERE RATHER THAN IMPORTED.
	 *
	 * FMaterialAttributeDefinitionMap (Engine/Private/Materials/MaterialAttributeDefinitionMap.cpp)
	 * registers EmissiveColor with a default of (0,0,0) and Opacity with a default of 1. That
	 * asymmetry is the whole reason the defaults are modelled at all: a blank unlit translucent
	 * material is not "invisible", it is BLACK AT FULL OPACITY, and a test that assumed a
	 * disconnected opacity meant zero would be right about the symptom for the wrong reason.
	 */
	constexpr float EngineDefaultOpacity = 1.0f;
	const FLinearColor EngineDefaultEmissive(0.0f, 0.0f, 0.0f, 0.0f);

	/**
	 * A HIGHLIGHT COLOUR HAS TO BE A COLOUR. The intended looks are amber for hover and cyan
	 * for selected; both have a channel at or near 1, so 0.05 is two orders of magnitude of
	 * headroom below anything anybody would pick on purpose and still firmly above "somebody
	 * left this at black". It is deliberately NOT an epsilon: a highlight at 0.001 is a
	 * highlight nobody can see, and passing it would be the same bug wearing a smaller number.
	 */
	constexpr float MinHighlightChannel = 0.05f;

	/**
	 * AND THE TWO HIGHLIGHTS HAVE TO BE TELLABLE APART, WHICH IS THE PROPERTY THE PLAYER
	 * ACTUALLY DEPENDS ON. The arithmetic, worked through before the number was chosen:
	 *
	 * Under BLEND_Translucent + MSM_Unlit the shader draws Emissive * Opacity + Dst * (1 - Opacity),
	 * so composited over a stand-in brick B the two looks are E1*O1 + B*(1-O1) and E2*O2 + B*(1-O2).
	 *
	 *   SHIPPED: amber (1, 0.55, 0.05) at 0.35 -> (0.4670, 0.3095, 0.1345)
	 *            cyan  (0.05, 0.8, 1.0) at 0.6 -> (0.1020, 0.5520, 0.6720)
	 *            largest channel difference 0.5375 on blue. Passes with 2.1x margin.
	 *
	 *   THE COPY-PASTE FIX: the same amber at 0.35 and at 0.6 -> (0.4670, 0.3095, 0.1345) and
	 *             (0.6720, 0.4020, 0.1020), largest channel difference 0.205 on red. FAILS, and
	 *             that is the intent — duplicating M_BrickHover, renaming it and changing only
	 *             the alpha is exactly the fix that leaves a player unable to tell what is
	 *             selected from what is merely under the cursor. If two shades of one hue are
	 *             ever wanted deliberately, this number is the one to argue with.
	 *
	 * THIS CONSTANT IS ABOUT THE PAIR AND NOTHING ELSE. Two overlays can differ from each other
	 * by half a channel and both be invisible on a brick — MinChannelChangeOverBareBrick below is
	 * the separate claim, argued separately, and neither number may be reused for the other.
	 */
	constexpr float MinDistinguishableChannel = 0.25f;

	/**
	 * AND EACH HIGHLIGHT HAS TO DIFFER FROM AN UNHIGHLIGHTED BRICK, WHICH IS THE CLAIM THE TWO
	 * ABOVE BOTH LEAVE OUT. "It paints a colour" is about the emissive alone and "they are
	 * tellable apart" is about the two overlays against each other; a pair can satisfy both and
	 * still leave the wall looking exactly as it did, which is the entire point of a highlight.
	 *
	 * THE QUANTITY, AND IT SIMPLIFIES EXACTLY. Composite minus background is
	 * E*O + B*(1-O) - B = O*(E - B) per channel, so what is being asserted is opacity times
	 * contrast — and neither factor alone is sufficient, which is why this cannot be re-expressed
	 * as a floor on either one.
	 *
	 *   SHIPPED, over the 0.18 stand-in: amber's |E - B| is (0.82, 0.37, 0.13), widest 0.82, so
	 *             the change is 0.35 * 0.82 = 0.287 — a 2.9x margin, and the tighter of the two.
	 *             Cyan's |E - B| is (0.13, 0.62, 0.82), so 0.6 * 0.82 = 0.492, a 4.9x margin.
	 *
	 * WHY 0.10 AND NOT AN EPSILON. These are LINEAR values, so the number has to be read at the
	 * display: 0.18 encodes to sRGB 0.4614 and 0.28 to 0.5658, a change of about 26 of 255
	 * display levels — unmistakable at a glance on a moving camera. That is deliberately far
	 * above a just-noticeable difference, because a highlight the player has to hunt for has
	 * already failed at the one job it has, which is answering "which bricks am I about to
	 * delete" BEFORE Delete is pressed. Read against the shipped emissives, whose contrast
	 * against the stand-in is 0.82 in both cases, it is also a floor of 0.10 / 0.82 = 0.122 on
	 * effective opacity: an overlay drawn at under about an eighth of full strength is not a
	 * highlight.
	 *
	 *   WHAT IT CATCHES, and it is why the row exists at all: M_BrickHover's opacity nudged to
	 *             0.02 during a look-and-feel pass gives 0.02 * 0.82 = 0.0164, a composite of
	 *             (0.1964, 0.1874, 0.1774) against a bare (0.18, 0.18, 0.18) — 1.6%, about 5
	 *             display levels, invisible in game. EVERY OTHER ASSERTION IN THIS FILE PASSES
	 *             ON IT: the brightest emissive channel is still 1.0 >= 0.05, the opacity 0.02
	 *             is still inside (0, 1], and the hover-versus-selected difference is still
	 *             0.4946 >= 0.25. A green suite over an invisible highlight is the same shape of
	 *             hole as the Nanite bug, and this is the assertion that closes it.
	 */
	constexpr float MinChannelChangeOverBareBrick = 0.10f;

	/**
	 * THE BRICK UNDERNEATH IS A FIXED STAND-IN, not the real one, because reading the brick's own
	 * material would make every assertion here depend on a third asset — a re-tint of the brick
	 * would then fail the HIGHLIGHT test. 0.18 is the conventional mid-grey.
	 *
	 * IT IS A REAL TERM IN BOTH COMPARISONS RATHER THAN A SPECTATOR, and an earlier version of
	 * this comment claimed the opposite ("any single background works"). It does not: the
	 * background cancels from a pair difference only when the two opacities are EQUAL, since
	 *
	 *     (E1*O1 + B*(1-O1)) - (E2*O2 + B*(1-O2)) = E1*O1 - E2*O2 + B*(O2 - O1)
	 *
	 * and the shipped opacities are 0.35 and 0.6, so the brick contributes 0.25*B = 0.045 to
	 * every channel of that difference. It does not change the verdict — blue would need B > 1.33
	 * before the pair dropped under MinDistinguishableChannel, and a brick brighter than white is
	 * not a brick — but the reason matters, because MinChannelChangeOverBareBrick measures
	 * AGAINST this background rather than across it, where the whole quantity is O*(E - B) and a
	 * stand-in chosen near either highlight colour would drive it to zero.
	 */
	const FLinearColor StandInBrickColour(0.18f, 0.18f, 0.18f, 1.0f);

	/** The two assets, by the constants the GAME resolves — never a re-typed literal. */
	struct FHighlightMaterialRow
	{
		const TCHAR* State;
		const TCHAR* Path;
	};

	/*
	 * ALL NINE, AND THE SIX NEIGHBOURS ARE HERE FOR THE PAIRWISE CLAIM RATHER THAN FOR THEIR OWN.
	 *
	 * Each of them individually only has to paint something and to differ from a bare brick, which
	 * is the same claim the first three make. What is new with six is that they must differ from
	 * EACH OTHER: the neighbour colours exist so a player can tell joint row 2's brick from joint
	 * row 4's, and two rows that draw the same hue are a lie about the one thing the swatch says.
	 * Because the loop below is pairwise over the whole table, adding them checks all 36 pairs —
	 * including each neighbour against hover, selected and inspected, which is the collision a
	 * palette chosen in isolation walks straight into.
	 */
	const FHighlightMaterialRow HighlightMaterials[] = {
		{ TEXT("Hovered"),    DestructionContent::BrickHoverMaterialPath },
		{ TEXT("Selected"),   DestructionContent::BrickSelectedMaterialPath },
		{ TEXT("Inspected"),  DestructionContent::BrickInspectedMaterialPath },
		{ TEXT("Neighbour0"), DestructionContent::BrickNeighbourMaterialPaths[0] },
		{ TEXT("Neighbour1"), DestructionContent::BrickNeighbourMaterialPaths[1] },
		{ TEXT("Neighbour2"), DestructionContent::BrickNeighbourMaterialPaths[2] },
		{ TEXT("Neighbour3"), DestructionContent::BrickNeighbourMaterialPaths[3] },
		{ TEXT("Neighbour4"), DestructionContent::BrickNeighbourMaterialPaths[4] },
		{ TEXT("Neighbour5"), DestructionContent::BrickNeighbourMaterialPaths[5] }
	};

	/** What one material input folded down to, plus where the value came from for a message. */
	struct FResolvedInput
	{
		FLinearColor Value = FLinearColor(0.0f, 0.0f, 0.0f, 0.0f);
		bool bResolved = false;
		FString Source;
	};

	/**
	 * The component mask an input may carry, applied the way HLSL swizzles do: the selected
	 * channels pack down from .R, and a single selected channel replicates.
	 */
	FLinearColor ApplyChannelMask(const FExpressionInput& Input, const FLinearColor& Value)
	{
		if (Input.Mask == 0)
		{
			return Value;
		}

		const float Source[4] = { Value.R, Value.G, Value.B, Value.A };
		const int32 Selected[4] = { Input.MaskR, Input.MaskG, Input.MaskB, Input.MaskA };

		float Picked[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		int32 Count = 0;

		for (int32 Channel = 0; Channel < 4; ++Channel)
		{
			if (Selected[Channel] != 0)
			{
				Picked[Count++] = Source[Channel];
			}
		}

		if (Count == 0)
		{
			return FLinearColor(0.0f, 0.0f, 0.0f, 0.0f);
		}

		if (Count == 1)
		{
			return FLinearColor(Picked[0], Picked[0], Picked[0], Picked[0]);
		}

		FLinearColor Masked(0.0f, 0.0f, 0.0f, 0.0f);
		float* const Destination[4] = { &Masked.R, &Masked.G, &Masked.B, &Masked.A };

		for (int32 Channel = 0; Channel < Count; ++Channel)
		{
			*Destination[Channel] = Picked[Channel];
		}

		return Masked;
	}

	bool FoldInput(const FExpressionInput& Input, int32 Depth, FLinearColor& Out, FString& OutWhere);

	/**
	 * FOLD ONE NODE DOWN TO A COLOUR, OR REFUSE.
	 *
	 * AN INDEPENDENT ORACLE RATHER THAN A CALL INTO THE ENGINE'S COMPILER. Nothing in this
	 * project evaluates a material graph, so there is no production algorithm to mirror; this
	 * is derived from the node classes' own declared value properties, and it agrees with the
	 * shader only if the asset really does carry the colour it claims to.
	 *
	 * IT FAILS CLOSED ON ANYTHING IT DOES NOT UNDERSTAND. A node type not listed here is
	 * reported as unresolvable and the test goes red naming the class, rather than being
	 * quietly treated as black or as visible. A highlight overlay is two constants; if it ever
	 * legitimately needs a texture or a time-varying node, this evaluator is the thing to
	 * extend, and the failure message says so.
	 */
	bool FoldExpression(const UMaterialExpression* Expression, int32 Depth, FLinearColor& Out, FString& OutWhere)
	{
		if (Expression == nullptr)
		{
			OutWhere = TEXT("nothing");
			return false;
		}

		/* Bounded rather than cycle-detected: the editor refuses a cyclic graph, and this is belt and braces. */
		if (Depth > 16)
		{
			OutWhere = TEXT("a graph deeper than this test will walk");
			return false;
		}

		if (const UMaterialExpressionConstant3Vector* const Constant3 = Cast<UMaterialExpressionConstant3Vector>(Expression))
		{
			Out = FLinearColor(Constant3->Constant.R, Constant3->Constant.G, Constant3->Constant.B, 1.0f);
			OutWhere = TEXT("a Constant3Vector");
			return true;
		}

		if (const UMaterialExpressionConstant4Vector* const Constant4 = Cast<UMaterialExpressionConstant4Vector>(Expression))
		{
			Out = Constant4->Constant;
			OutWhere = TEXT("a Constant4Vector");
			return true;
		}

		if (const UMaterialExpressionConstant2Vector* const Constant2 = Cast<UMaterialExpressionConstant2Vector>(Expression))
		{
			Out = FLinearColor(Constant2->R, Constant2->G, 0.0f, 0.0f);
			OutWhere = TEXT("a Constant2Vector");
			return true;
		}

		if (const UMaterialExpressionConstant* const Scalar = Cast<UMaterialExpressionConstant>(Expression))
		{
			/* A scalar feeding a float3 replicates, which is what the shader compiler does too. */
			Out = FLinearColor(Scalar->R, Scalar->R, Scalar->R, Scalar->R);
			OutWhere = TEXT("a Constant");
			return true;
		}

		if (const UMaterialExpressionVectorParameter* const VectorParameter = Cast<UMaterialExpressionVectorParameter>(Expression))
		{
			Out = VectorParameter->DefaultValue;
			OutWhere = TEXT("a VectorParameter's default");
			return true;
		}

		if (const UMaterialExpressionScalarParameter* const ScalarParameter = Cast<UMaterialExpressionScalarParameter>(Expression))
		{
			Out = FLinearColor(
				ScalarParameter->DefaultValue, ScalarParameter->DefaultValue,
				ScalarParameter->DefaultValue, ScalarParameter->DefaultValue);
			OutWhere = TEXT("a ScalarParameter's default");
			return true;
		}

		if (const UMaterialExpressionMultiply* const Multiply = Cast<UMaterialExpressionMultiply>(Expression))
		{
			FLinearColor A(Multiply->ConstA, Multiply->ConstA, Multiply->ConstA, Multiply->ConstA);
			FLinearColor B(Multiply->ConstB, Multiply->ConstB, Multiply->ConstB, Multiply->ConstB);

			if (Multiply->A.Expression != nullptr && !FoldInput(Multiply->A, Depth + 1, A, OutWhere))
			{
				return false;
			}

			if (Multiply->B.Expression != nullptr && !FoldInput(Multiply->B, Depth + 1, B, OutWhere))
			{
				return false;
			}

			Out = FLinearColor(A.R * B.R, A.G * B.G, A.B * B.B, A.A * B.A);
			OutWhere = TEXT("a Multiply of constants");
			return true;
		}

		if (const UMaterialExpressionAdd* const Add = Cast<UMaterialExpressionAdd>(Expression))
		{
			FLinearColor A(Add->ConstA, Add->ConstA, Add->ConstA, Add->ConstA);
			FLinearColor B(Add->ConstB, Add->ConstB, Add->ConstB, Add->ConstB);

			if (Add->A.Expression != nullptr && !FoldInput(Add->A, Depth + 1, A, OutWhere))
			{
				return false;
			}

			if (Add->B.Expression != nullptr && !FoldInput(Add->B, Depth + 1, B, OutWhere))
			{
				return false;
			}

			Out = FLinearColor(A.R + B.R, A.G + B.G, A.B + B.B, A.A + B.A);
			OutWhere = TEXT("an Add of constants");
			return true;
		}

		OutWhere = FString::Printf(
			TEXT("a %s, which this test cannot fold to a colour"), *Expression->GetClass()->GetName());

		return false;
	}

	bool FoldInput(const FExpressionInput& Input, int32 Depth, FLinearColor& Out, FString& OutWhere)
	{
		/* GetTracedInput steps through reroute nodes, which carry no value of their own. */
		const FExpressionInput Traced = Input.GetTracedInput();

		FLinearColor Value(0.0f, 0.0f, 0.0f, 0.0f);

		if (!FoldExpression(Traced.Expression, Depth, Value, OutWhere))
		{
			return false;
		}

		Out = ApplyChannelMask(Traced, Value);
		return true;
	}

	/* FLinearColor has no scalar constructor; these two adapt the colour and scalar input types. */
	FLinearColor AsColour(const FLinearColor& Value) { return Value; }
	FLinearColor AsColour(float Value) { return FLinearColor(Value, Value, Value, Value); }

	/**
	 * The order here is the compiler's own, from FColorMaterialInput::CompileWithDefault /
	 * FScalarMaterialInput::CompileWithDefault: an inline constant wins, then the connected
	 * expression, then the property's registered default.
	 */
	template <typename InputType>
	FResolvedInput ResolveInput(const InputType& Input, const FLinearColor& PropertyDefault)
	{
		FResolvedInput Resolved;

		/*
		 * RECORDED RATHER THAN FIXED, BECAUSE IT IS DEAD AND A BLIND FIX WOULD BE UNTESTED EITHER
		 * WAY. FColorMaterialInput::Constant is an FColor, so this line reaches AsColour through
		 * FLinearColor's FColor constructor — which is FromSRGBColor, an sRGB->LINEAR DECODE — and
		 * an inline (255, 140, 13) would be reported as roughly (1.0, 0.26, 0.005) rather than as
		 * the 8-bit value divided by 255. Both shipped assets fold from a Constant3Vector, so
		 * UseConstant is false on every input this test reads and nothing exercises this branch at
		 * all. If anyone ever ticks the inline constant on an input, check this against
		 * FColorMaterialInput::CompileWithDefault before believing the number it prints.
		 */
		if (Input.UseConstant)
		{
			Resolved.Value = AsColour(Input.Constant);
			Resolved.bResolved = true;
			Resolved.Source = TEXT("an inline constant on the input");
			return Resolved;
		}

		if (Input.Expression == nullptr)
		{
			Resolved.Value = PropertyDefault;
			Resolved.bResolved = true;
			Resolved.Source = TEXT("NOTHING CONNECTED, so the engine's default for the property");
			return Resolved;
		}

		FString Where;

		if (FoldInput(Input, 0, Resolved.Value, Where))
		{
			Resolved.bResolved = true;
			Resolved.Source = Where;
			return Resolved;
		}

		Resolved.bResolved = false;
		Resolved.Source = Where;
		return Resolved;
	}

	float LargestChannel(const FLinearColor& Colour)
	{
		return FMath::Max3(Colour.R, Colour.G, Colour.B);
	}

	float LargestChannelDifference(const FLinearColor& A, const FLinearColor& B)
	{
		return FMath::Max3(
			FMath::Abs(A.R - B.R),
			FMath::Abs(A.G - B.G),
			FMath::Abs(A.B - B.B));
	}

	bool IsFiniteColour(const FLinearColor& Colour)
	{
		return FMath::IsFinite(Colour.R) && FMath::IsFinite(Colour.G) && FMath::IsFinite(Colour.B);
	}

	/** What an unlit translucent overlay leaves on the screen over a given background. */
	FLinearColor CompositeOverBrick(const FLinearColor& Emissive, float Opacity)
	{
		return FLinearColor(
			Emissive.R * Opacity + StandInBrickColour.R * (1.0f - Opacity),
			Emissive.G * Opacity + StandInBrickColour.G * (1.0f - Opacity),
			Emissive.B * Opacity + StandInBrickColour.B * (1.0f - Opacity),
			1.0f);
	}

	const TCHAR* BlendModeName(EBlendMode Mode)
	{
		switch (Mode)
		{
		case BLEND_Opaque:         return TEXT("Opaque");
		case BLEND_Masked:         return TEXT("Masked");
		case BLEND_Translucent:    return TEXT("Translucent");
		case BLEND_Additive:       return TEXT("Additive");
		case BLEND_Modulate:       return TEXT("Modulate");
		case BLEND_AlphaComposite: return TEXT("AlphaComposite");
		case BLEND_AlphaHoldout:   return TEXT("AlphaHoldout");
		default:                   return TEXT("some other blend mode");
		}
	}

	FString DescribeColour(const FLinearColor& Colour)
	{
		return FString::Printf(TEXT("(%.4f, %.4f, %.4f)"), Colour.R, Colour.G, Colour.B);
	}
}

/**
 * THE HIGHLIGHT MATERIALS ACTUALLY PAINT SOMETHING, EACH PAIR PAINTS TWO DIFFERENT THINGS, AND
 * EACH PAINTS SOMETHING DIFFERENT FROM A BARE BRICK.
 *
 * GREEN ON ARRIVAL, AND IT SAYS SO RATHER THAN IMPLYING IT DROVE ANYTHING. Both assets are
 * correct: two expression nodes each, amber (1, 0.55, 0.05) at 0.35 and cyan (0.05, 0.8, 1.0) at
 * 0.6, unlit and translucent. This is a net, not a red step. An earlier version of this comment
 * asserted as present-tense fact that both were saved with an EMPTY graph and a black emissive;
 * that was never true, and where the belief came from is written down at the node-count comment
 * further down — UMaterial has no reflected `Expressions` property in 5.8, so a Python dump
 * reports zero nodes for a graph that is plainly full. A reader who trusted the old header and a
 * reader who trusted that comment reached opposite conclusions about the same two assets.
 *
 * WHY World.Brick.HighlightWearsAMaterial IS NOT ENOUGH, and it is the same gap shape as the
 * missing-world-push bug: every link is asserted individually, and what can still be wrong lives
 * one step past the last assertion. That test asserts the brick asks for the right ASSET per
 * state, including that Hovered and Selected are different assets. It says nothing at all about
 * whether either asset draws.
 *
 * NOT A NODE COUNT, DELIBERATELY. "The graph is not empty" is satisfied by one stray node wired
 * to nothing, which draws exactly as much as no nodes at all. What is asserted here is the VALUE
 * the two inputs that govern an unlit translucent overlay fold down to, walked from the material
 * property inputs themselves — so a disconnected input is not a missing node, it is the engine's
 * registered default, and it is judged as the colour it will actually be.
 *
 * FOUR CLAIMS, IN THE ORDER THEY DEPEND ON EACH OTHER.
 *
 * THE PAIRING IS PINNED FIRST, AND IT IS A PRECONDITION RATHER THAN A PREFERENCE. Unlit plus
 * Translucent is exactly what makes EmissiveColor and Opacity the two inputs that decide what
 * lands on the screen. Change to Default Lit and the colour comes from BaseColor under a light;
 * change to Masked and Opacity stops being read at all. Either way the two assertions below
 * would go on reading two inputs that no longer govern and would silently stop meaning anything
 * — which is worse than failing. So the pairing is asserted, and its message says that changing
 * it deliberately means rewriting the rest of this test rather than deleting this row.
 *
 * THE OVERLAY DRAWS A COLOUR. Emissive folds to something that is not black and opacity folds
 * to something above zero, both finite. Both halves are needed and neither implies the other: a
 * brilliant amber at zero opacity and a black at full opacity are both invisible highlights.
 *
 * AND THE OVERLAY CHANGES THE BRICK IT IS DRAWN ON. The two above are both satisfied by an
 * overlay that leaves the wall looking exactly as it did — a real emissive at an opacity of 0.02
 * clears every one of them and moves the brick by 1.6%. That is a green suite over an invisible
 * highlight, which is the identical symptom to the Nanite bug next door, so the composite is
 * compared against the bare brick PER MATERIAL rather than only across the pair. Its constant is
 * argued on its own terms beside MinChannelChangeOverBareBrick and deliberately shares nothing
 * with the pair threshold, which was argued for a different comparison.
 *
 * AND THE TWO ARE TELLABLE APART ON THE SAME BRICK. This is the property the player depends on,
 * and it is the one a plausible fix breaks: duplicate the hover material, rename it, change the
 * alpha, ship two ambers. The existing test's "different assets" assertion passes that happily.
 * Composited over one stand-in brick, the two looks must differ by a quarter of a channel; the
 * arithmetic for the intended pair and for the copy-paste fix is worked through beside the
 * constant.
 *
 * PARAMETERISED OVER THE MATERIALS, so a further highlight state is a row rather than a test —
 * and Inspected arrived as exactly that row. The pair comparison is PAIRWISE over the rows rather
 * than one hardcoded comparison, because "each differs from the one before it" is satisfied by a
 * third overlay that draws exactly like the first.
 *
 * THE PATHS COME FROM DestructionContent, never a re-typed literal — a test that hardcodes the
 * path stops testing the asset the game actually loads the moment the constant moves.
 *
 * NEEDS A TICKING WORLD: no, and no world at all. This reads two assets off disk; it costs a
 * load and some arithmetic.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHighlightMaterialPaintTest,
	"DestructionGame.Content.HighlightMaterialsPaintSomething",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FHighlightMaterialPaintTest::RunTest(const FString& Parameters)
{
	using namespace HighlightMaterialPaintTestSupport;

	constexpr int32 RowCount = UE_ARRAY_COUNT(HighlightMaterials);

	FLinearColor Composite[RowCount] = {};
	bool bUsable[RowCount] = {};

	for (int32 Row = 0; Row < RowCount; ++Row)
	{
		const FHighlightMaterialRow& Expectation = HighlightMaterials[Row];

		UMaterialInterface* const Loaded =
			LoadObject<UMaterialInterface>(nullptr, Expectation.Path);

		TestNotNull(
			*FString::Printf(TEXT("%s: '%s' must resolve to a material"),
				Expectation.State, Expectation.Path),
			Loaded);

		if (Loaded == nullptr)
		{
			continue;
		}

		/*
		 * A BASE MATERIAL, because the graph is what is being read. A material instance carries
		 * parameter overrides this evaluator would not see, so accepting one would mean reading
		 * the parent's defaults and reporting them as the instance's colour. Both assets are
		 * base materials today; if one ever becomes an instance this refuses rather than lies.
		 */
		UMaterial* const Material = Cast<UMaterial>(Loaded);

		TestNotNull(
			*FString::Printf(
				TEXT("%s: '%s' must be a base UMaterial for its graph to be readable here; it is a %s"),
				Expectation.State, Expectation.Path, *Loaded->GetClass()->GetName()),
			Material);

		if (Material == nullptr)
		{
			continue;
		}

		/*
		 * THE PRECONDITION. If this row fails, every assertion after it is reading two inputs
		 * that no longer decide anything — do not delete it, rewrite the rest of the test for
		 * whatever pairing replaced it.
		 */
		const EBlendMode BlendMode = Material->GetBlendMode();

		TestTrue(
			*FString::Printf(
				TEXT("%s: '%s' must be Translucent for Opacity to govern what it draws; it is %s. If this changed deliberately, the emissive/opacity assertions below need rewriting, not deleting"),
				Expectation.State, Expectation.Path, BlendModeName(BlendMode)),
			BlendMode == BLEND_Translucent);

		const FMaterialShadingModelField ShadingModels = Material->GetShadingModels();

		TestTrue(
			*FString::Printf(
				TEXT("%s: '%s' must be Unlit for EmissiveColor to be the whole of its colour; its shading model field is 0x%04x and MSM_Unlit is bit 0. If this changed deliberately, the emissive/opacity assertions below need rewriting, not deleting"),
				Expectation.State, Expectation.Path, ShadingModels.GetShadingModelField()),
			ShadingModels.HasShadingModel(MSM_Unlit));

		const UMaterialEditorOnlyData* const Graph = Material->GetEditorOnlyData();

		TestNotNull(
			*FString::Printf(TEXT("%s: '%s' must carry editor-only graph data to be readable here"),
				Expectation.State, Expectation.Path),
			Graph);

		if (Graph == nullptr)
		{
			continue;
		}

		const FResolvedInput Emissive = ResolveInput(Graph->EmissiveColor, EngineDefaultEmissive);
		const FResolvedInput Opacity = ResolveInput(Graph->Opacity, AsColour(EngineDefaultOpacity));

		/*
		 * THE NODE COUNT IS REPORTED AND NOTHING IS ASSERTED ABOUT IT, deliberately, and it is
		 * here because reading it from the wrong place is a live trap. UMaterial has no reflected
		 * `Expressions` property in 5.8 — the graph moved to GetEditorOnlyData()->ExpressionCollection
		 * — so a Python dump asking a material for `expressions` reports zero for a graph that is
		 * plainly full. That reading is what these two assets were once believed blank on. It is
		 * printed so the next person comparing the two readings sees them side by side.
		 */
		AddInfo(FString::Printf(
			TEXT("%s ('%s'): %d expression node(s); emissive %s from %s; opacity %.4f from %s"),
			Expectation.State, Expectation.Path,
			Material->GetExpressions().Num(),
			*DescribeColour(Emissive.Value), *Emissive.Source,
			Opacity.Value.R, *Opacity.Source));

		TestTrue(
			*FString::Printf(
				TEXT("%s: '%s' emissive input must fold to a colour this test can read; it is %s"),
				Expectation.State, Expectation.Path, *Emissive.Source),
			Emissive.bResolved);

		TestTrue(
			*FString::Printf(
				TEXT("%s: '%s' opacity input must fold to a value this test can read; it is %s"),
				Expectation.State, Expectation.Path, *Opacity.Source),
			Opacity.bResolved);

		if (!Emissive.bResolved || !Opacity.bResolved)
		{
			continue;
		}

		/* Fail closed on a degenerate value rather than letting a NaN read as a plausible colour. */
		TestTrue(
			*FString::Printf(
				TEXT("%s: '%s' must fold to finite values; emissive is %s and opacity is %.4f"),
				Expectation.State, Expectation.Path,
				*DescribeColour(Emissive.Value), Opacity.Value.R),
			IsFiniteColour(Emissive.Value) && FMath::IsFinite(Opacity.Value.R));

		if (!IsFiniteColour(Emissive.Value) || !FMath::IsFinite(Opacity.Value.R))
		{
			continue;
		}

		/*
		 * THE DIRECT STATEMENT OF THE BUG. An unlit translucent overlay draws its emissive at
		 * its opacity, so a black emissive is a highlight with no colour in it however the
		 * opacity is set.
		 */
		TestTrue(
			*FString::Printf(
				TEXT("%s: '%s' must paint a colour — its brightest emissive channel must reach %.2f, it is %s (from %s)"),
				Expectation.State, Expectation.Path, MinHighlightChannel,
				*DescribeColour(Emissive.Value), *Emissive.Source),
			LargestChannel(Emissive.Value) >= MinHighlightChannel);

		TestTrue(
			*FString::Printf(
				TEXT("%s: '%s' must be drawn at all — its opacity must be above 0 and at most 1, it is %.4f (from %s)"),
				Expectation.State, Expectation.Path, Opacity.Value.R, *Opacity.Source),
			Opacity.Value.R > 0.0f && Opacity.Value.R <= 1.0f);

		Composite[Row] = CompositeOverBrick(Emissive.Value, Opacity.Value.R);
		bUsable[Row] = true;

		/*
		 * AND IT HAS TO CHANGE THE BRICK. The two assertions above are about the overlay's own
		 * numbers; this one is about what the player sees, and it is the only one that fails for
		 * a real colour drawn at an opacity nobody would notice. The quantity reduces exactly to
		 * Opacity * |Emissive - Brick| per channel, so it is the product of the two things the
		 * assertions above check separately — which is why neither of them implies it.
		 */
		TestTrue(
			*FString::Printf(
				TEXT("%s: '%s' must LOOK different from an unhighlighted brick, not merely paint a colour: over a %s brick it draws %s, moving the widest channel by %.4f where %.2f is needed. Its emissive is %s at opacity %.4f"),
				Expectation.State, Expectation.Path,
				*DescribeColour(StandInBrickColour),
				*DescribeColour(Composite[Row]),
				LargestChannelDifference(Composite[Row], StandInBrickColour),
				MinChannelChangeOverBareBrick,
				*DescribeColour(Emissive.Value), Opacity.Value.R),
			LargestChannelDifference(Composite[Row], StandInBrickColour) >= MinChannelChangeOverBareBrick);
	}

	/*
	 * AND EVERY PAIR OF LOOKS DIFFERS, PAIRWISE RATHER THAN IN A CHAIN. Held back until all the
	 * materials have been read so a failure names both composites at once; guarded rather than
	 * assumed, because a row that could not be read has already failed above and a second failure
	 * about a colour it never had would point at the wrong thing.
	 *
	 * PAIRWISE IS THE WHOLE POINT WITH MORE THAN TWO ROWS. "Each differs from the one before it"
	 * is satisfied by a third highlight that draws exactly like the FIRST, which is the shape the
	 * copy-paste fix takes as soon as there is something older than the previous asset to copy.
	 */
	for (int32 Left = 0; Left < RowCount; ++Left)
	{
		for (int32 Right = Left + 1; Right < RowCount; ++Right)
		{
			if (!bUsable[Left] || !bUsable[Right])
			{
				AddError(FString::Printf(
					TEXT("%s and %s must both be readable before their looks can be compared — see the failures above"),
					HighlightMaterials[Left].State, HighlightMaterials[Right].State));

				continue;
			}

			TestTrue(
				*FString::Printf(
					TEXT("%s and %s must LOOK different on the same brick, not merely be different assets: over a %s brick they draw %s and %s, differing by %.4f in the widest channel where %.2f is needed"),
					HighlightMaterials[Left].State, HighlightMaterials[Right].State,
					*DescribeColour(StandInBrickColour),
					*DescribeColour(Composite[Left]), *DescribeColour(Composite[Right]),
					LargestChannelDifference(Composite[Left], Composite[Right]), MinDistinguishableChannel),
				LargestChannelDifference(Composite[Left], Composite[Right]) >= MinDistinguishableChannel);
		}
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
