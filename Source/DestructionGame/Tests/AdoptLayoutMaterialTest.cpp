// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Layout.h"
#include "Core/StructureBinding.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Profiles/MaterialProfiles.h"
#include "Core/Structure.h"

#include "Components/StaticMeshComponent.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * SHED_PATH.md Phase F / slice F0 — CARRY A PIECE'S MATERIAL from a built layout into the
 * live played world, so the cross-material weakest-link physics (Phase B, B1/B2/B3) is
 * actually LIVE in play and not silently inert.
 *
 * BEHAVIOUR UNDER TEST, in one sentence: when a layout whose pieces carry materials is
 * adopted into a live FStructureBinding, the adopted structure's cross-material joint reads
 * the paired weakest-link EffectiveJointStrength — i.e. the material a piece is made of
 * survives AdoptLayout end-to-end, not just its mass and grounding.
 *
 * WHY THIS IS THE F0 RED. B3 (commit 156e099) wired FStructure::EffectiveJointStrength off
 * FStructurePiece::Material, and CrossMaterialBearingWiringTest proves it on a HAND-BUILT
 * FStructure. But the play pipeline reaches the live world through FBrickLayout ->
 * AdoptLayout -> FStructureBinding, and AdoptLayout (Core/StructureBinding.cpp) copies only
 *
 *     Out.AddPiece(Piece.MassKg, Piece.bIsGrounded, Actors[PieceIndex], Layout.Boxes[PieceIndex]);
 *
 * per piece — NOT Piece.Material. (Joint strength profiles DO survive: FConnection is copied
 * whole by AddConnection, so a joint's OWN FConnectionStrength is intact — only the per-piece
 * MATERIAL is lost.) The consequence is that a shed built through the scenario/binding pipeline
 * behaves as if every piece were the bare connection (all one material): the wood-on-brick
 * bearings and post-on-footing crushes the B3 unit test passes on go INERT in the played game.
 *
 * THE FIXTURE — a Timber post bearing on a grounded ClayBrick footing through an Unbreakable
 * bed joint, laid on the layout's OWN FStructure and each face tagged with its material, then
 * adopted. This is CrossMaterialBearingWiringTest's fixture put through the play path:
 *
 *        +----------+     Post: Timber (f_c,0 = 21 MPa), ungrounded.
 *        +==========+  <- BED JOINT, Unbreakable connection (compressive 1e12 MPa):
 *        +----------+     the CONNECTION cannot govern, so the MATERIAL crush must.
 *        | FOOTING  |     Footing: ClayBrick (f_c = 20 MPa), grounded.
 *        +==========+
 *
 * WHY THE UNBREAKABLE JOINT. Its compressive strength (1e12 MPa) sits eleven orders of
 * magnitude above either material, so the bare-connection reading (what a dropped material
 * leaves behind) and the wired weakest-link reading (the brick's 20 MPa) cannot be confused:
 * the assertion is a clean discriminator, not a numeric coincidence.
 *
 * WHY 20, NOT 21. The crush is min over both faces and the connection:
 * min(1e12, timber 21, brick 20) = 20, the weaker of the two MATERIALS. Reaching 20 through
 * the adopted structure proves BOTH faces' materials survived adoption — the weaker one, even.
 *
 * THE STRONGEST ASSERTION (the one that proves the physics is live, not just a pointer): the
 * ADOPTED structure's EffectiveJointStrength(bed).CompressiveStrengthMPa == 20. Today AdoptLayout
 * drops the material, so the adopted piece's Material is nullptr, EffectiveJointStrength falls
 * back to the bare connection, and it reads 1e12 — the missing behaviour, not a broken fixture.
 * The pointer-level mechanism (adopted Material == nullptr vs the layout's own == &Timber) is
 * asserted too, as the right-reason witness.
 *
 * THE POSITIVE CONTROL. Before adoption, the layout's OWN FStructure already reads 20 through
 * EffectiveJointStrength — so the drop is unambiguously AdoptLayout's, not a mis-tagged fixture.
 *
 * NEEDS A TICKING WORLD: NO. Nothing is solved and nothing is ticked; every assertion is on
 * the adopted graph's static strength pairing. Stand-in UObjects in the transient package
 * stand in for the brick actors, exactly as StructureBindingTest.cpp's AdoptLayout test does.
 *
 * NAMED NAMESPACE, not anonymous: a unity build merges files into one translation unit.
 */
namespace AdoptLayoutMaterialTestSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	/** Every piece is this deep on Y; with a 10 cm face that is a 98 cm2 bed. Value is immaterial here. */
	constexpr double WytheWidthCm = 9.8;
	constexpr double FaceLengthCm = 10.0;
	constexpr double JointThicknessCm = 1.0;

	/** Masses are immaterial to a static strength-pairing readout; a real-ish number is less distracting. */
	constexpr double FootingMassKg = 50.0;
	constexpr double PostMassKg = 60.0;

	FPieceBox MakeBox(double CentreZ, double SizeZ)
	{
		FPieceBox Box;
		Box.ExtentCm = FVector(FaceLengthCm, WytheWidthCm, SizeZ) * 0.5;
		Box.CentreCm = FVector(0.0, 0.0, CentreZ);
		return Box;
	}

	/** A rooted stand-in for a brick actor: AdoptLayout only holds it, so any UObject serves. */
	UObject* MakeStandIn()
	{
		UObject* StandIn = NewObject<UStaticMeshComponent>(GetTransientPackage());
		StandIn->AddToRoot();
		return StandIn;
	}

	void ReleaseStandIns(const TArray<UObject*>& StandIns)
	{
		for (UObject* StandIn : StandIns)
		{
			if (StandIn != nullptr)
			{
				StandIn->RemoveFromRoot();
			}
		}
	}

	/**
	 * Lay the grounded footing and the post that bears on it into the LAYOUT's own FStructure,
	 * tag each face with its material, and append one box per piece so the layout's two arrays
	 * stay in step (AdoptLayout refuses a desynced one). Returns the bed joint handle, or
	 * INDEX_NONE if the interface could not be formed.
	 */
	int32 BuildLayout(
		FBrickLayout& OutLayout,
		const FMaterialProfile& FootingMaterial,
		const FMaterialProfile& PostMaterial,
		int32& OutFooting,
		int32& OutPost)
	{
		const FPieceBox FootBox = MakeBox(/*Z*/ 10.0, /*SizeZ*/ 20.0);
		const FPieceBox PostBox = MakeBox(/*Z*/ 31.0, /*SizeZ*/ 20.0);

		OutFooting = OutLayout.Structure.AddPiece(FootingMassKg, /*bIsGrounded*/ true, FootBox.CentreCm);
		OutPost = OutLayout.Structure.AddPiece(PostMassKg, /*bIsGrounded*/ false, PostBox.CentreCm);

		OutLayout.Boxes.Add(FootBox);
		OutLayout.Boxes.Add(PostBox);

		OutLayout.Structure.SetPieceMaterial(OutFooting, &FootingMaterial);
		OutLayout.Structure.SetPieceMaterial(OutPost, &PostMaterial);

		FConnection Joint;
		if (!MakeInterface(OutFooting, FootBox, OutPost, PostBox, JointThicknessCm, Unbreakable, Joint))
		{
			return INDEX_NONE;
		}

		return OutLayout.Structure.AddConnection(Joint);
	}
}

/**
 * A piece's material survives AdoptLayout, so the adopted cross-material joint reads the
 * weakest-link EffectiveJointStrength rather than the bare connection.
 *
 * NEEDS A TICKING WORLD: NO. See the file header.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAdoptLayoutCarriesPieceMaterialTest,
	"DestructionGame.Acceptance.CrossMaterialBearing.AdoptLayoutCarriesPieceMaterial",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FAdoptLayoutCarriesPieceMaterialTest::RunTest(const FString& Parameters)
{
	using namespace DestructionProfiles;
	using namespace AdoptLayoutMaterialTestSupport;

	/* ------------------------------------------------------------------ *
	 * FIXTURE PRECONDITIONS — the hand-derived crush only means 20 while
	 * the profiles carry the strengths it was derived against, and while
	 * the connection genuinely cannot govern.
	 * ------------------------------------------------------------------ */
	TestTrue(
		FString::Printf(TEXT("PRECONDITION: the Unbreakable connection's compressive (%g MPa) must dwarf both "
			"materials, so the MATERIAL crush governs"), Unbreakable.CompressiveStrengthMPa),
		Unbreakable.CompressiveStrengthMPa > 1.0e9);

	TestTrue(
		FString::Printf(TEXT("PRECONDITION: timber compressive must be 21 MPa, profile carries %g"),
			Timber.Strength.CompressiveStrengthMPa),
		Timber.Strength.CompressiveStrengthMPa == 21.0);

	TestTrue(
		FString::Printf(TEXT("PRECONDITION: clay brick compressive must be 20 MPa, profile carries %g"),
			ClayBrick.Strength.CompressiveStrengthMPa),
		ClayBrick.Strength.CompressiveStrengthMPa == 20.0);

	/*
	 * The weakest link on the compression axis: min over the connection and both faces. The
	 * brick (20) is the weaker MATERIAL, so it governs — NOT the timber (21), and emphatically
	 * not the connection (1e12). Reaching 20 through the adopted structure proves both faces
	 * survived.
	 */
	const double MaterialCrushMPa = FMath::Min3(
		Unbreakable.CompressiveStrengthMPa,
		Timber.Strength.CompressiveStrengthMPa,
		ClayBrick.Strength.CompressiveStrengthMPa);                                    // 20

	TestTrue(
		FString::Printf(TEXT("PRECONDITION: the weakest-link crush must be the brick's 20 MPa, got %g"),
			MaterialCrushMPa),
		MaterialCrushMPa == 20.0);

	/* ---- BUILD THE LAYOUT: a Timber post on a grounded ClayBrick footing. ---- */
	FBrickLayout Layout;
	int32 Footing = INDEX_NONE;
	int32 Post = INDEX_NONE;
	const int32 BedJoint = BuildLayout(Layout, /*Footing*/ ClayBrick, /*Post*/ Timber, Footing, Post);

	if (BedJoint == INDEX_NONE)
	{
		AddError(TEXT("FIXTURE: the producer must emit the bed joint"));
		return false;
	}

	TestEqual(TEXT("FIXTURE: two pieces — the grounded footing and the post"),
		Layout.Structure.NumPieces(), 2);
	TestEqual(TEXT("FIXTURE: one joint — the cross-material bed bearing"),
		Layout.Structure.NumConnections(), 1);
	TestEqual(TEXT("FIXTURE: one box per piece, or AdoptLayout refuses the layout"),
		Layout.Boxes.Num(), Layout.Structure.NumPieces());

	/* ------------------------------------------------------------------ *
	 * POSITIVE CONTROL — the layout's OWN structure already reads the crush.
	 * If this failed the fixture would be mis-tagged and the adoption result
	 * meaningless; passing it pins the drop below on AdoptLayout alone.
	 * ------------------------------------------------------------------ */
	TestTrue(
		FString::Printf(TEXT("CONTROL: the layout's footing must carry ClayBrick before adoption")),
		Layout.Structure.GetPiece(Footing).Material == &ClayBrick);
	TestTrue(
		FString::Printf(TEXT("CONTROL: the layout's post must carry Timber before adoption")),
		Layout.Structure.GetPiece(Post).Material == &Timber);

	const double LayoutCrushMPa =
		Layout.Structure.EffectiveJointStrength(BedJoint).CompressiveStrengthMPa;

	AddInfo(FString::Printf(
		TEXT("CONTROL: layout EffectiveJointStrength compressive = %.12g MPa (expect the material crush %.12g)"),
		LayoutCrushMPa, MaterialCrushMPa));

	TestTrue(
		FString::Printf(TEXT("CONTROL: the layout's own joint must already read the weakest-link crush (%g MPa), "
			"got %.12g — the fixture is correctly tagged before adoption"), MaterialCrushMPa, LayoutCrushMPa),
		FMath::IsNearlyEqual(LayoutCrushMPa, MaterialCrushMPa, 1.0e-6));

	/* ---- ADOPT THROUGH THE PLAY PATH. ---- */
	TArray<UObject*> StandIns;
	StandIns.Add(MakeStandIn());
	StandIns.Add(MakeStandIn());

	FStructureBinding Binding;
	Binding.StructureId = 7;

	const bool bAdopted = AdoptLayout(Layout, StandIns, Binding);

	TestTrue(TEXT("AdoptLayout should adopt this well-formed layout"), bAdopted);

	if (!bAdopted)
	{
		ReleaseStandIns(StandIns);
		return false;
	}

	/* The adopted graph must at least be the right shape before its strengths mean anything. */
	TestEqual(TEXT("adoption should carry both pieces"),
		Binding.GetStructure().NumPieces(), 2);
	TestEqual(TEXT("adoption should carry the bed joint"),
		Binding.GetStructure().NumConnections(), 1);

	/* ------------------------------------------------------------------ *
	 * THE MECHANISM WITNESS — the material pointer itself. Today AdoptLayout
	 * copies mass/grounded/actor/box and drops Material, so the adopted piece
	 * reads nullptr where the layout read &Timber / &ClayBrick. This is the
	 * right-reason proof: the strength falls back to the bare connection
	 * because the material is gone, not because of an arithmetic error.
	 * ------------------------------------------------------------------ */
	const DestructionProfiles::FMaterialProfile* AdoptedFootingMaterial =
		Binding.GetStructure().GetPiece(Footing).Material;
	const DestructionProfiles::FMaterialProfile* AdoptedPostMaterial =
		Binding.GetStructure().GetPiece(Post).Material;

	AddInfo(FString::Printf(
		TEXT("ADOPTED materials: footing %s, post %s (layout had ClayBrick / Timber)"),
		AdoptedFootingMaterial == &ClayBrick ? TEXT("ClayBrick")
			: (AdoptedFootingMaterial == nullptr ? TEXT("nullptr") : TEXT("<other>")),
		AdoptedPostMaterial == &Timber ? TEXT("Timber")
			: (AdoptedPostMaterial == nullptr ? TEXT("nullptr") : TEXT("<other>"))));

	TestTrue(
		TEXT("MECHANISM: the adopted footing must still carry ClayBrick — AdoptLayout must copy Piece.Material"),
		AdoptedFootingMaterial == &ClayBrick);
	TestTrue(
		TEXT("MECHANISM: the adopted post must still carry Timber — AdoptLayout must copy Piece.Material"),
		AdoptedPostMaterial == &Timber);

	/* ------------------------------------------------------------------ *
	 * THE STRONGEST ASSERTION — the physics is LIVE on the adopted graph.
	 * EffectiveJointStrength pairs the connection with both faces' materials;
	 * with the materials carried across it reads the brick's 20 MPa crush,
	 * with them dropped it reads the bare Unbreakable connection (1e12).
	 * ------------------------------------------------------------------ */
	const double AdoptedCrushMPa =
		Binding.GetStructure().EffectiveJointStrength(BedJoint).CompressiveStrengthMPa;

	AddInfo(FString::Printf(
		TEXT("ADOPTED EffectiveJointStrength compressive = %.12g MPa. Carried-through expects %.12g "
			"(material crush); a dropped material reads the bare connection %.3g"),
		AdoptedCrushMPa, MaterialCrushMPa, Unbreakable.CompressiveStrengthMPa));

	TestTrue(
		FString::Printf(TEXT("F0: the ADOPTED cross-material joint must read the weakest-link crush (%g MPa), "
			"got %.12g. A dropped-material adoption reads the bare connection (~%.3g), which makes the shed's "
			"wood-on-brick physics inert in play"),
			MaterialCrushMPa, AdoptedCrushMPa, Unbreakable.CompressiveStrengthMPa),
		FMath::IsNearlyEqual(AdoptedCrushMPa, MaterialCrushMPa, 1.0e-6));

	ReleaseStandIns(StandIns);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
