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
	 * ACTUALLY DEPENDS ON.
	 *
	 * THE VALUE IS UNCHANGED AND ITS DERIVATION IS ENTIRELY REPLACED, which is worth stating
	 * plainly so nobody reads the surviving 0.25 as evidence the old argument held. The old one
	 * ran over Emissive*Opacity + B*(1 - Opacity) and was set by the margin between the shipped
	 * pair (0.5375) and the copy-paste fix (0.205); both of those quantities are now wrong. See
	 * the header for the measurement that replaced the model.
	 *
	 * WHAT A PAIR DIFFERENCE IS UNDER ADDITIVE. Composite is min(1, E + B) per channel, so an
	 * UNCLAMPED pair difference is (E1 + B) - (E2 + B) = E1 - E2 exactly: the background cancels
	 * completely and opacity never enters. A pair comparison is therefore a statement about the
	 * two EMISSIVES and nothing else — until a channel reaches white, at which point the clamp
	 * stops being an identity and the background decides the answer again. That is the whole
	 * reason two backgrounds are modelled below rather than one.
	 *
	 * WHAT 0.25 BUYS, in the units the claim is actually about. A linear separation converts to
	 * display levels WORST at the top of the range, because sRGB compresses highlights: the
	 * minimum over x in [0, 0.75] of sRGB(x + 0.25) - sRGB(x) is 30.4 of 255 levels, at x = 0.75.
	 * From the 0.18 stand-in it is 57.6 levels and from the lit brick's blue at 0.304 it is 46.5.
	 * So 0.25 linear is a floor of about THIRTY display levels wherever the pair happens to sit —
	 * roughly thirty times a just-noticeable difference, which is the margin two bricks metres
	 * apart, at different angles, on a moving camera actually need.
	 *
	 *   THE COPY-PASTE FIX NO LONGER SETS THIS NUMBER, and cannot. Duplicating M_BrickHover,
	 *             renaming it and changing only the alpha used to land at 0.205 and fail by a
	 *             whisker. Under additive it lands at EXACTLY 0.0000 in every channel at every
	 *             background, because opacity is not in the composite: two overlays sharing one
	 *             emissive are one look. Any positive threshold catches it, so it is no longer
	 *             the case this constant is calibrated against.
	 *
	 *   THE SHIPPED PALETTE FAILED THIS TWICE AND NO LONGER DOES, AND WHAT MOVED WAS THE PALETTE.
	 *             Neither threshold was touched, which is the order this file insists on: if a
	 *             palette cannot satisfy an honest additive model, the palette is what changes.
	 *             The two failures were Hovered (1.00, 0.55, 0.05) against a Neighbour0 that was
	 *             then a second amber at (1.00, 0.62, 0.10), separating by 0.0700 on green at both
	 *             backgrounds — about 9 display levels, with everything that used to distinguish
	 *             them being the opacity ladder; and Selected (0.05, 0.80, 1.00) against a
	 *             Neighbour5 that was then a teal at (0.00, 0.55, 0.45), separating by 0.3700 over
	 *             the stand-in but only 0.2460 over the lit brick as cyan's blue clamped from
	 *             1.304 to 1.0 and its green from 1.020 to 1.0. The repick answered both: slot 0
	 *             is now a green (0.00, 0.80, 0.00), slot 2 a clay (0.45, 0.10, 0.00) and slot 5
	 *             a sage (0.20, 0.45, 0.05), and all thirty-six pairs clear 0.25 at both
	 *             backgrounds.
	 *
	 *   THE TIGHTEST PAIR TODAY PASSES BY 1.32x, which is close enough to be worth writing down
	 *             rather than discovering later: Neighbour0 against Neighbour5 sits at 0.3300 on
	 *             green over the lit brick, where the green channel is the wash again — 0.80 plus
	 *             the brick's 0.220 clamps to 1.0, which is what costs the pair the 0.0200 it has
	 *             over the stand-in. Nothing else in the table comes under 1.40x. Swept over
	 *             uniform grey backgrounds this is the worst pair at every brightness from 0.18
	 *             up and the FIRST to go under, at a grey just past 0.30; it is therefore the pair
	 *             a measured sunlit brick would be expected to break.
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
	 * THE QUANTITY STILL SIMPLIFIES, TO SOMETHING MUCH WEAKER THAN IT USED TO. Composite minus
	 * background is min(1, E + B) - B = min(E, 1 - B) per channel: the emissive itself, capped by
	 * the HEADROOM the background leaves. The old form, O*(E - B), was read off alpha blending
	 * and is wrong in both factors — opacity is not in the composite at all, and the background
	 * enters as a ceiling rather than as contrast.
	 *
	 * SO THIS ROW IS NOW VERY NEARLY A RESTATEMENT OF MinHighlightChannel, AND SAYING SO IS THE
	 * HONEST THING TO DO. min(E, 1 - B) >= 0.10 is "some emissive channel reaches 0.10" plus "the
	 * background leaves it room". The second conjunct is the only independent content left, and it
	 * bites only against a background whose EVERY channel sits at or above 0.90 — a brick already
	 * within 0.10 of white, on which no additive overlay of any colour can show at all. Neither
	 * background modelled below is remotely that bright, so AGAINST THE TWO BACKGROUNDS THIS FILE
	 * CAN MODEL THIS ROW CATCHES NOTHING that MinHighlightChannel would not. It is kept because
	 * the ceiling is the shape of the failure a sunlit scene will actually produce, and the row is
	 * where that will be caught the day a bright measurement is available to add.
	 *
	 *   WHAT IT USED TO CATCH AND PROVABLY CANNOT ANY MORE: M_BrickHover's opacity nudged to 0.02
	 *             during a look-and-feel pass. Under alpha blending that gave a 1.6% move, about
	 *             five display levels, invisible in game — and this row was the only thing in the
	 *             file that failed on it. Under additive an overlay at 0.02 draws exactly as
	 *             brightly as one at 0.95, so there is no longer anything to catch: the bug that
	 *             motivated this constant is not a bug the renderer can have. That loss is real
	 *             and is not being papered over.
	 *
	 * WHY THE VALUE IS STILL 0.10. The perceptual claim is the one part of the old comment that
	 * survives the model change intact, because it was always argued at the display rather than in
	 * linear: 0.10 linear is 26.6 of 255 display levels up from the 0.18 stand-in, 25.6 from the
	 * lit brick's red at 0.195, 24.2 from its green at 0.220 and 20.6 from its blue at 0.304 —
	 * unmistakable at a glance on a moving camera, and deliberately far above a just-noticeable
	 * difference, because a highlight the player has to hunt for has already failed at the one job
	 * it has, which is answering "which bricks am I about to delete" BEFORE Delete is pressed. The
	 * per-level figures fall as the background rises, which is the general rule under additive and
	 * the reason the assertion is evaluated against every modelled background rather than the
	 * darkest.
	 */
	constexpr float MinChannelChangeOverBareBrick = 0.10f;

	/**
	 * THE BRICK UNDERNEATH IS MODELLED TWICE, AND THE SECOND ONE IS LOAD-BEARING.
	 *
	 * IT IS A STAND-IN RATHER THAN THE REAL BRICK MATERIAL, because reading the brick's own
	 * material would make every assertion here depend on a third asset — a re-tint of the brick
	 * would then fail the HIGHLIGHT test.
	 *
	 * THE FIRST ROW IS THE CONVENTIONAL 0.18 MID-GREY, AND IT IS THE OPTIMISTIC END OF THE RANGE.
	 * The second is a MEASURED lit brick from the sandbox, (0.195, 0.220, 0.304) linear, read off
	 * a render rather than chosen. It is barely brighter than the stand-in in red and green and
	 * noticeably brighter in blue, and that difference alone is enough to change a verdict.
	 *
	 * WHY A SECOND ONE IS NEEDED AT ALL, WHICH IS AN ARGUMENT THAT ONLY WORKS UNDER ADDITIVE.
	 * A pair difference is (E1 + B) - (E2 + B) = E1 - E2, so the background cancels exactly and
	 * any single background would do — RIGHT UP TO THE CLAMP. Once a channel of E + B reaches
	 * white the subtraction stops being an identity, and a brighter background pushes more
	 * channels into the ceiling: hues do not merely shift, they WASH TOWARD WHITE and toward each
	 * other. On the palette this row was added for, Selected against Neighbour5 separated by 0.3700
	 * over the stand-in and by 0.2460 over the lit brick, which was the difference between passing
	 * and failing — modelling one background is what let that palette read as fine while washing out
	 * on screen.
	 *
	 * THAT PAIR HAS SINCE BEEN REPICKED, AND THE SECOND ROW STILL CHANGES AN ANSWER RATHER THAN
	 * MERELY A NUMBER — but by less, and saying so is the honest thing to do. Today's tightest pair,
	 * Neighbour0 against Neighbour5, separates by 0.3500 over the stand-in and by 0.3300 over the lit
	 * brick, because Neighbour0's green plus the brick's 0.220 clamps to white. That is 1.40x against
	 * 1.32x: both pass, so the second background currently changes no verdict in the suite. It is the
	 * shape of the failure rather than a live catch, which is the same standing this file gives
	 * MinChannelChangeOverBareBrick.
	 *
	 * WHAT IS MISSING IS A THIRD, SUNLIT ROW, and it is missing because nobody has measured one —
	 * inventing a number here would be exactly the kind of agreeable arithmetic this file exists
	 * to prevent. The prediction it would test is written beside MinDistinguishableChannel:
	 * Neighbour0 against Neighbour5 goes under first, and over uniform grey it does so just past
	 * 0.30.
	 */
	struct FBackgroundRow
	{
		const TCHAR* Name;
		FLinearColor Colour;
	};

	const FBackgroundRow Backgrounds[] = {
		{ TEXT("the 0.18 stand-in grey"), FLinearColor(0.18f, 0.18f, 0.18f, 1.0f) },
		{ TEXT("a measured lit brick"),   FLinearColor(0.195f, 0.220f, 0.304f, 1.0f) }
	};

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

	/**
	 * WHAT THE OVERLAY PASS LEAVES ON THE SCREEN OVER A GIVEN BACKGROUND: THE BRICK PLUS THE
	 * EMISSIVE, CLAMPED AT WHITE. This is the measured model, not the documented one — see the
	 * header for the two independent lines that establish it, and for what the measurement does
	 * and does not settle.
	 *
	 * OPACITY IS NOT A PARAMETER, AND ITS ABSENCE IS THE POINT rather than an omission. It is not
	 * passed in at all, so nobody reading this can believe a term is being applied that is not.
	 *
	 * THE CLAMP IS THE DISPLAY'S WHITE CEILING and it is the only place the background survives a
	 * pair comparison. The framebuffer is float and does not clip at 1.0, so this is a model of
	 * the tonemapper rather than of the buffer — a conservative one, since a real tonemapper rolls
	 * off below 1.0 and therefore squeezes two bright hues together SOONER than this does.
	 *
	 * FMath::Min DISCARDS A NaN RATHER THAN PROPAGATING IT, so a NaN emissive would come out of
	 * here as a plausible colour. The caller asserts finiteness and skips the row BEFORE reaching
	 * this, which is what keeps that from mattering; do not reorder those two.
	 */
	FLinearColor CompositeOverBackground(const FLinearColor& Emissive, const FLinearColor& Background)
	{
		return FLinearColor(
			FMath::Min(1.0f, Emissive.R + Background.R),
			FMath::Min(1.0f, Emissive.G + Background.G),
			FMath::Min(1.0f, Emissive.B + Background.B),
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
 * THE OVERLAYS COMPOSITE ADDITIVELY, WHICH IS A MEASUREMENT AND NOT WHAT THE BLEND MODE SAYS.
 * The whole of this file used to derive from Emissive*Opacity + Dst*(1 - Opacity), which is what
 * BLEND_Translucent + MSM_Unlit is documented to do and is not what the renderer does to these
 * overlays. Measured as the mean of a 25x25 px patch at the centre of a tinted brick in a
 * 1920x1080 render, sRGB 0-255, for M_BrickNeighbour3 (emissive (1.00, 0.00, 0.12), opacity 0.95),
 * over a bare brick reading (122, 130, 150):
 *
 *     BLEND_TRANSLUCENT (shipped)   (255, 121, 208)   hot pink
 *     BLEND_ADDITIVE                (255, 156, 213)   hot pink, essentially unchanged
 *     BLEND_OPAQUE                  (126, 135, 154)   nothing; the overlay vanishes
 *
 * TWO INDEPENDENT LINES AGREE. ARITHMETIC: with the brick at linear (0.195, 0.220, 0.304), alpha
 * blending at 0.95 predicts sRGB (252, 29, 99) — the green channel would be all but obliterated,
 * since the emissive's green is 0 — while the measured green sits at 121, which is the BRICK'S OWN
 * green. An overlay that leaves a channel where it found it is adding, not blending. SUBSTITUTION:
 * switching the material to literal BLEND_ADDITIVE changed the image almost not at all, which
 * under genuine alpha blending at 0.95 would have been dramatic.
 *
 * SO THE MODEL HERE IS min(1, Emissive + Background) PER CHANNEL, and three consequences run
 * through every constant below.
 *
 * OPACITY IS NOT A BLEND FACTOR, SO IT IS NOT IN THE MODEL AT ALL. It is still read, still
 * required finite and inside (0, 1], and that is now an AUTHORING sanity check with no claimed
 * effect on the screen — not a term in any arithmetic. THE HOVER / SELECTED / INSPECTED "OPACITY
 * LADDER" AT 0.35 / 0.95 / 0.95 IS THEREFORE A FICTION: hover is not dimmer than selected, it is
 * the same brightness, and every place in this project that describes the ladder as a strength
 * ordering is describing something that does not happen.
 *
 * WHAT THE MEASUREMENT DOES NOT SETTLE, AND THE EXPERIMENT THAT WOULD. It does not discriminate
 * B + E from B + E*O, because it was taken on ONE material at opacity 0.95, whose green emissive
 * is 0 (so the opacity factor multiplies nothing) and whose red is clipped. At 0.95 the two models
 * differ by 5% of the emissive, which is inside the residual. THE DISCRIMINATING MEASUREMENT IS
 * M_BrickHover, the only asset at 0.35, and the convenient form of it has lapsed: it used to be
 * "patch it beside M_BrickNeighbour0, which is very nearly the same amber at 0.95", and the repick
 * made that slot a green — two hues cannot be compared for brightness by eye. What remains, and
 * holds the hue exactly fixed rather than nearly, is M_BrickHover against ITSELF with its opacity
 * set to 0.95 and nothing else touched. Equal brightness means opacity is inert; a visibly dimmer
 * 0.35 means it scales. Until that is taken, dropping opacity is the CONSERVATIVE reading for the
 * pair claim —
 * a pair "distinguished" only by brightness of one hue is precisely the copy-paste fix the pair
 * constant exists to catch — and the OPTIMISTIC one for the bare-brick claim, which is argued
 * beside MinChannelChangeOverBareBrick and is nearly vacuous under either reading.
 *
 * AND NO OVERLAY CAN EVER DARKEN A BRICK, WHICH IS A STANDING CONSTRAINT ON FUTURE PALETTES
 * RATHER THAN AN OBSERVATION ABOUT THIS ONE. Under addition every composite is the brick PLUS
 * something, so a "dark" highlight — a deep red, a near-black outline, anything that reads as a
 * shadow — is unreachable by any emissive whatsoever, and every hue on offer is a brightening. A
 * negative emissive channel is the only way to express one, and it is rejected below rather than
 * silently clamped away, because a clamp would turn "this palette cannot be drawn" into "this
 * palette is fine".
 *
 * IT ARRIVED RED AND THE PALETTE IS WHAT MOVED, WHICH IS THE ORDER THAT MATTERS. Under the
 * corrected model the shipped palette failed two of its thirty-six pairs; neither threshold was
 * moved to produce that and neither was moved to remove it — slots 0, 2 and 5 were repicked
 * instead, and every pair now clears at both backgrounds. The rule stands for the next palette: if
 * one cannot satisfy an honest additive model, the palette is what changes. The old failures, the
 * repick and today's tightest pair are all beside MinDistinguishableChannel.
 *
 * ALL NINE ASSETS ARE OTHERWISE CORRECT: two expression nodes each, a Constant3Vector emissive and
 * a Constant opacity, unlit and translucent. An earlier version of this comment asserted as
 * present-tense fact that they were saved with an EMPTY graph and a black emissive; that was never
 * true, and where the belief came from is written down at the node-count comment further down —
 * UMaterial has no reflected `Expressions` property in 5.8, so a Python dump reports zero nodes
 * for a graph that is plainly full.
 *
 * WHY World.Brick.HighlightWearsAMaterial IS NOT ENOUGH, and it is the same gap shape as the
 * missing-world-push bug: every link is asserted individually, and what can still be wrong lives
 * one step past the last assertion. That test asserts the brick asks for the right ASSET per
 * state, including that Hovered and Selected are different assets. It says nothing at all about
 * whether either asset draws.
 *
 * NOT A NODE COUNT, DELIBERATELY. "The graph is not empty" is satisfied by one stray node wired
 * to nothing, which draws exactly as much as no nodes at all. What is asserted here is the VALUE
 * the inputs fold down to, walked from the material property inputs themselves — so a
 * disconnected input is not a missing node, it is the engine's registered default, and it is
 * judged as the colour it will actually be.
 *
 * FIVE CLAIMS, IN THE ORDER THEY DEPEND ON EACH OTHER.
 *
 * THE PAIRING IS PINNED FIRST, AND ITS REASON HAS CHANGED WITH THE MODEL. Unlit is what makes
 * EmissiveColor the whole of the overlay's colour, and that argument is untouched — change to
 * Default Lit and the colour comes from BaseColor under a light. Translucent is no longer required
 * "so that Opacity governs", because opacity governs nothing; it is required because BLEND_OPAQUE
 * WAS MEASURED TO DRAW NOTHING AT ALL — the patch read (126, 135, 154) against a bare (122, 130,
 * 150), i.e. the overlay vanished. The overlay pass emits a draw only for a translucent material,
 * so opaque is not available as a fix for anything, and a material switched to it silently stops
 * being a highlight while every value in it stays correct.
 *
 * THE OVERLAY DRAWS A COLOUR. Emissive folds to something that is not black, and it is finite and
 * non-negative in every channel — the second half is the never-darken constraint made assertable,
 * since a negative channel is the only expressible way to ask for a highlight that cannot exist.
 *
 * AND THE OVERLAY CHANGES THE BRICK IT IS DRAWN ON, against every modelled background. This is
 * kept and its teeth are honestly reported as gone: under additive it no longer catches the
 * invisible-highlight bug it was written for, because opacity cannot dim an overlay. What it now
 * watches is the CEILING — a brick close enough to white that nothing can be added to it — and the
 * argument for keeping it despite catching nothing today is beside MinChannelChangeOverBareBrick.
 *
 * AND THE TWO ARE TELLABLE APART ON THE SAME BRICK. This is the property the player depends on and
 * the one this file caught the palette out on: it held two ambers whose only distinction was an
 * opacity that does not reach the screen. Composited over each modelled background, two looks must
 * differ by a quarter of a channel; the arithmetic, the two failures that repick answered and the
 * 1.32x pair that is now the tightest are worked through beside the constant.
 *
 * PARAMETERISED OVER THE MATERIALS AND OVER THE BACKGROUNDS, so a further highlight state is a row
 * rather than a test — and Inspected arrived as exactly that row — and a measured sunlit brick is
 * likewise one row rather than a second set of assertions. The pair comparison is PAIRWISE over the
 * rows rather than one hardcoded comparison, because "each differs from the one before it" is
 * satisfied by a third overlay that draws exactly like the first.
 *
 * THE PATHS COME FROM DestructionContent, never a re-typed literal — a test that hardcodes the
 * path stops testing the asset the game actually loads the moment the constant moves.
 *
 * NEEDS A TICKING WORLD: no, and no world at all. This reads nine assets off disk; it costs a
 * load and some arithmetic. It also renders no pixel — the compositing model it folds against is
 * a written-down MEASUREMENT of the renderer, not an observation this process can make, so the
 * day the model changes again it changes here by hand.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHighlightMaterialPaintTest,
	"DestructionGame.Content.HighlightMaterialsPaintSomething",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FHighlightMaterialPaintTest::RunTest(const FString& Parameters)
{
	using namespace HighlightMaterialPaintTestSupport;

	constexpr int32 RowCount = UE_ARRAY_COUNT(HighlightMaterials);
	constexpr int32 BackgroundCount = UE_ARRAY_COUNT(Backgrounds);

	FLinearColor Composite[RowCount][BackgroundCount] = {};
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
		 * THE PRECONDITION, AND ITS REASON IS MEASURED RATHER THAN DOCUMENTED. The overlay pass
		 * emits a draw only for a translucent material: switching this material to BLEND_OPAQUE
		 * was measured to make the highlight VANISH, with every value in the asset unchanged. So
		 * this is not "Opacity governs" — nothing governs but the emissive — it is "there is a
		 * draw at all". If this row fails, do not delete it; rewrite the rest of the test for
		 * whatever pairing replaced it.
		 */
		const EBlendMode BlendMode = Material->GetBlendMode();

		TestTrue(
			*FString::Printf(
				TEXT("%s: '%s' must be Translucent for the overlay pass to draw it at all — Opaque was measured to draw nothing; it is %s. If this changed deliberately, the emissive assertions below need rewriting, not deleting"),
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
		 * THE DIRECT STATEMENT OF THE BUG. An unlit overlay adds its emissive to the brick, so a
		 * black emissive is a highlight with no colour in it and there is no opacity that rescues
		 * it — under addition, nothing multiplies the emissive back up either.
		 */
		TestTrue(
			*FString::Printf(
				TEXT("%s: '%s' must paint a colour — its brightest emissive channel must reach %.2f, it is %s (from %s)"),
				Expectation.State, Expectation.Path, MinHighlightChannel,
				*DescribeColour(Emissive.Value), *Emissive.Source),
			LargestChannel(Emissive.Value) >= MinHighlightChannel);

		/*
		 * THE NEVER-DARKEN CONSTRAINT, MADE ASSERTABLE. A composite is the brick PLUS the emissive,
		 * so no highlight can be darker than the brick it is on — a negative channel is the only
		 * way to write one down, and the clamp in CompositeOverBackground would quietly turn it
		 * into zero and report a perfectly ordinary colour. Checked here rather than left to the
		 * model, because "this palette cannot be drawn" must not read as "this palette is fine".
		 */
		TestTrue(
			*FString::Printf(
				TEXT("%s: '%s' must not ask to DARKEN the brick — an additive overlay can only add, so no emissive channel may be negative; it is %s (from %s)"),
				Expectation.State, Expectation.Path,
				*DescribeColour(Emissive.Value), *Emissive.Source),
			Emissive.Value.R >= 0.0f && Emissive.Value.G >= 0.0f && Emissive.Value.B >= 0.0f);

		/*
		 * OPACITY IS AN AUTHORING SANITY CHECK AND NOTHING MORE, and the message says so rather
		 * than claiming an effect the renderer was measured not to have. It is deliberately kept:
		 * a zero or a 1.5 is still a sign somebody meant something by it, and the day the
		 * discriminating hover measurement in the header is taken, this is where the answer lands.
		 */
		TestTrue(
			*FString::Printf(
				TEXT("%s: '%s' opacity must be a sane authoring value, above 0 and at most 1, it is %.4f (from %s). It does NOT dim the overlay — the composite was measured to be additive and opacity is not a factor in it"),
				Expectation.State, Expectation.Path, Opacity.Value.R, *Opacity.Source),
			Opacity.Value.R > 0.0f && Opacity.Value.R <= 1.0f);

		bUsable[Row] = true;

		/*
		 * AND IT HAS TO CHANGE THE BRICK, AGAINST EVERY MODELLED BACKGROUND. The quantity is
		 * min(Emissive, 1 - Background) per channel — the emissive capped by the headroom — so the
		 * background can only ever take this DOWN, and the darkest background is the one that would
		 * flatter the palette. Asserted per background for that reason, and because a failure that
		 * names which brick it washed out against is the one worth reading.
		 */
		for (int32 Background = 0; Background < BackgroundCount; ++Background)
		{
			const FBackgroundRow& Bare = Backgrounds[Background];

			Composite[Row][Background] = CompositeOverBackground(Emissive.Value, Bare.Colour);

			const float Change = LargestChannelDifference(Composite[Row][Background], Bare.Colour);

			TestTrue(
				*FString::Printf(
					TEXT("%s: '%s' must LOOK different from an unhighlighted brick, not merely paint a colour: over %s %s it draws %s, moving the widest channel by %.4f where %.2f is needed. Its emissive is %s"),
					Expectation.State, Expectation.Path,
					Bare.Name, *DescribeColour(Bare.Colour),
					*DescribeColour(Composite[Row][Background]),
					Change, MinChannelChangeOverBareBrick,
					*DescribeColour(Emissive.Value)),
				Change >= MinChannelChangeOverBareBrick);
		}
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
	 *
	 * AND ONCE PER BACKGROUND, WHICH IS NOT REDUNDANT UNDER ADDITION EVEN THOUGH THE BACKGROUND
	 * CANCELS. It cancels from the subtraction and not from the clamp, so a pair that separates
	 * cleanly on a dark brick can wash together on a bright one; Selected against Neighbour5 is
	 * exactly that case and it is why both rows are checked rather than the darker one.
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

			for (int32 Background = 0; Background < BackgroundCount; ++Background)
			{
				const FBackgroundRow& Bare = Backgrounds[Background];

				const float Separation = LargestChannelDifference(
					Composite[Left][Background], Composite[Right][Background]);

				TestTrue(
					*FString::Printf(
						TEXT("%s and %s must LOOK different on the same brick, not merely be different assets: over %s %s they draw %s and %s, differing by %.4f in the widest channel where %.2f is needed"),
						HighlightMaterials[Left].State, HighlightMaterials[Right].State,
						Bare.Name, *DescribeColour(Bare.Colour),
						*DescribeColour(Composite[Left][Background]),
						*DescribeColour(Composite[Right][Background]),
						Separation, MinDistinguishableChannel),
					Separation >= MinDistinguishableChannel);
			}
		}
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
