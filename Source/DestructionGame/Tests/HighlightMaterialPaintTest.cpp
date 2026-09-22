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

// Editor-only: the material graph lives in editor-only data, so a cooked build has nothing to read.
#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

// Uniquely named: unity builds merge files.
namespace HighlightMaterialPaintTestSupport
{
	/**
	 * Engine defaults for unconnected inputs (FMaterialAttributeDefinitionMap): emissive black,
	 * opacity 1. So a blank unlit translucent material draws black at full opacity, not invisible.
	 */
	constexpr float EngineDefaultOpacity = 1.0f;
	const FLinearColor EngineDefaultEmissive(0.0f, 0.0f, 0.0f, 0.0f);

	/**
	 * Minimum brightest emissive channel. Intended colours have a channel near 1; 0.05 only catches
	 * "left at black". Not an epsilon: a 0.001 highlight is invisible.
	 */
	constexpr float MinHighlightChannel = 0.05f;

	/**
	 * Minimum widest-channel difference between any two highlights on the same brick. Under the
	 * additive model min(1, E + B) the background cancels until a channel clips at white, which is
	 * why two backgrounds are modelled.
	 *
	 * 0.25 linear is at least ~30 of 255 sRGB display levels anywhere in range. Tightest pair today:
	 * Neighbour0 vs Neighbour5 at 0.3300 over the lit brick (green clips), 1.32x the threshold; over
	 * uniform grey it fails just past 0.30. This is the pair claim only; do not reuse it for
	 * MinChannelChangeOverBareBrick.
	 */
	constexpr float MinDistinguishableChannel = 0.25f;

	/**
	 * Minimum widest-channel change a highlight makes to a bare brick: min(E, 1 - B) per channel.
	 * Nearly a restatement of MinHighlightChannel; it only bites on a background within 0.10 of
	 * white, which neither modelled background is. Kept because that ceiling is how a sunlit scene
	 * would fail. 0.10 linear is ~21-27 display levels over the modelled backgrounds.
	 */
	constexpr float MinChannelChangeOverBareBrick = 0.10f;

	/**
	 * Stand-in backgrounds (not the brick material, so a brick re-tint can't fail this test): the
	 * conventional 0.18 grey, and a lit brick measured from a sandbox render. The second matters
	 * because a brighter background clips more channels to white and pulls hues together; it once
	 * turned a passing pair (0.3700) into a failing one (0.2460). Today it changes no verdict.
	 *
	 * A sunlit row is missing because none has been measured; the prediction is beside
	 * MinDistinguishableChannel.
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

	/** A highlight asset, by the path constant the game resolves, never a re-typed literal. */
	struct FHighlightMaterialRow
	{
		const TCHAR* State;
		const TCHAR* Path;
	};

	/*
	 * All nine highlight assets. The loop is pairwise, so all 36 pairs are checked, including each
	 * neighbour colour against hover, selected and inspected.
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

	/** A material input's folded value, and where it came from (for messages). */
	struct FResolvedInput
	{
		FLinearColor Value = FLinearColor(0.0f, 0.0f, 0.0f, 0.0f);
		bool bResolved = false;
		FString Source;
	};

	/** Apply an input's component mask like an HLSL swizzle: selected channels pack from .R; a single one replicates. */
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
	 * Fold one node to a colour from the node classes' value properties (an independent oracle, not
	 * the engine compiler). Fails closed: an unlisted node type is unresolvable and the test names
	 * its class. Extend this if a highlight ever legitimately needs another node type.
	 */
	bool FoldExpression(const UMaterialExpression* Expression, int32 Depth, FLinearColor& Out, FString& OutWhere)
	{
		if (Expression == nullptr)
		{
			OutWhere = TEXT("nothing");
			return false;
		}

		// Depth bound as a safety net; the editor already refuses cyclic graphs.
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
			// A scalar feeding a float3 replicates, as in the shader compiler.
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
		// Step through reroute nodes.
		const FExpressionInput Traced = Input.GetTracedInput();

		FLinearColor Value(0.0f, 0.0f, 0.0f, 0.0f);

		if (!FoldExpression(Traced.Expression, Depth, Value, OutWhere))
		{
			return false;
		}

		Out = ApplyChannelMask(Traced, Value);
		return true;
	}

	// Adapt colour and scalar input types (FLinearColor has no scalar constructor).
	FLinearColor AsColour(const FLinearColor& Value) { return Value; }
	FLinearColor AsColour(float Value) { return FLinearColor(Value, Value, Value, Value); }

	/**
	 * Resolve an input in the compiler's order (F*MaterialInput::CompileWithDefault): inline
	 * constant, then connected expression, then the property default.
	 */
	template <typename InputType>
	FResolvedInput ResolveInput(const InputType& Input, const FLinearColor& PropertyDefault)
	{
		FResolvedInput Resolved;

		/*
		 * Unexercised branch, left unfixed: FColorMaterialInput::Constant is an FColor, and its
		 * FLinearColor conversion is an sRGB decode, not /255. No shipped asset uses an inline
		 * constant; if one does, check this against CompileWithDefault first.
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
	 * On-screen overlay colour: brick plus emissive, clamped at white (the measured additive model;
	 * see the test's comment). Opacity is deliberately not a parameter. The clamp models the
	 * tonemapper's white ceiling; a real one rolls off earlier, so this is optimistic about bright hues.
	 *
	 * FMath::Min discards NaN, so a NaN emissive would come out plausible. The caller rejects
	 * non-finite rows before calling this; keep that order.
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
 * Each highlight material paints a visible colour, changes a bare brick, and differs from every
 * other highlight. World.Brick.HighlightWearsAMaterial only checks the brick picks the right asset,
 * not that the asset draws.
 *
 * The overlays composite additively, min(1, Emissive + Background), which was measured rather than
 * taken from the blend mode. M_BrickNeighbour3 (emissive (1.00, 0.00, 0.12), opacity 0.95) over a
 * bare brick (122, 130, 150) sRGB read (255, 121, 208) as Translucent, nearly the same as Additive,
 * and (126, 135, 154) as Opaque. Alpha blending would have crushed green to ~29; it stayed at the
 * brick's own 121.
 *
 * Consequences:
 * - Opacity is not in the model; it is only checked as a sane authoring value. The 0.35/0.95/0.95
 *   hover/selected/inspected "opacity ladder" has no visible effect. The measurement cannot
 *   distinguish B + E from B + E*O at 0.95; M_BrickHover at 0.35 versus itself at 0.95 would.
 * - No overlay can darken a brick, so negative emissive channels are rejected rather than clamped.
 * - Translucent is still required because Opaque was measured to draw nothing.
 *
 * The palette failed two pairs under this model; slots 0, 2 and 5 were repicked and no threshold
 * moved. If a future palette fails, change the palette.
 *
 * Asserts on folded input values, not node counts (a stray unconnected node proves nothing).
 * Paths come from DestructionContent. Needs no world; renders nothing, so if the renderer's
 * compositing changes, update the model by hand.
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

		// Must be a base material: this evaluator would not see an instance's parameter overrides.
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
		 * Translucent is required because the overlay pass only draws translucent materials (Opaque
		 * was measured to vanish). If this fails deliberately, rewrite the test; don't delete this.
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
		 * Node count is reported, not asserted. In 5.8 UMaterial has no reflected `Expressions`
		 * property, so a Python dump reports zero nodes for a full graph; this shows the real count.
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

		// Fail closed on NaN/inf rather than letting it read as a plausible colour.
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

		// A black emissive adds nothing to the brick, whatever the opacity.
		TestTrue(
			*FString::Printf(
				TEXT("%s: '%s' must paint a colour — its brightest emissive channel must reach %.2f, it is %s (from %s)"),
				Expectation.State, Expectation.Path, MinHighlightChannel,
				*DescribeColour(Emissive.Value), *Emissive.Source),
			LargestChannel(Emissive.Value) >= MinHighlightChannel);

		/*
		 * An additive overlay cannot darken. Reject negative channels here, since the model's clamp
		 * would otherwise hide them.
		 */
		TestTrue(
			*FString::Printf(
				TEXT("%s: '%s' must not ask to DARKEN the brick — an additive overlay can only add, so no emissive channel may be negative; it is %s (from %s)"),
				Expectation.State, Expectation.Path,
				*DescribeColour(Emissive.Value), *Emissive.Source),
			Emissive.Value.R >= 0.0f && Emissive.Value.G >= 0.0f && Emissive.Value.B >= 0.0f);

		// Opacity is an authoring sanity check only; it has no measured effect on screen.
		TestTrue(
			*FString::Printf(
				TEXT("%s: '%s' opacity must be a sane authoring value, above 0 and at most 1, it is %.4f (from %s). It does NOT dim the overlay — the composite was measured to be additive and opacity is not a factor in it"),
				Expectation.State, Expectation.Path, Opacity.Value.R, *Opacity.Source),
			Opacity.Value.R > 0.0f && Opacity.Value.R <= 1.0f);

		bUsable[Row] = true;

		/*
		 * The change is min(Emissive, 1 - Background) per channel, so brighter backgrounds only
		 * reduce it. Asserted per background so a failure names the one it washed out on.
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
	 * Every pair must differ, pairwise rather than in a chain (a chain misses a third row copied
	 * from the first). Checked per background because the white clamp can wash a pair together on
	 * a brighter brick. Unreadable rows already failed above, so they are reported, not compared.
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
