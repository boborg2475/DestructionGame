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
 * SHED_PATH.md Phase F, slice F0: a piece's material survives AdoptLayout, so cross-material
 * weakest-link strength is live in play, not only on hand-built structures.
 *
 * Fixture: a Timber post (f_c 29) on a grounded ClayBrick footing (f_c 20) through an Unbreakable
 * bed joint (1e12 MPa). The weakest link is min(1e12, 29, 20) = 20, so the adopted joint reads 20
 * only if both materials survive; with materials dropped it reads the bare 1e12. The layout's own
 * structure is checked first as a positive control, and the adopted Material pointers are asserted
 * as the mechanism. Stand-in UObjects replace brick actors, as in StructureBindingTest.cpp.
 */
namespace AdoptLayoutMaterialTestSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	constexpr double WytheWidthCm = 9.8;
	constexpr double FaceLengthCm = 10.0;
	constexpr double JointThicknessCm = 1.0;

	// Masses do not affect a strength-pairing readout.
	constexpr double FootingMassKg = 50.0;
	constexpr double PostMassKg = 60.0;

	FPieceBox MakeBox(double CentreZ, double SizeZ)
	{
		FPieceBox Box;
		Box.ExtentCm = FVector(FaceLengthCm, WytheWidthCm, SizeZ) * 0.5;
		Box.CentreCm = FVector(0.0, 0.0, CentreZ);
		return Box;
	}

	/** A rooted stand-in for a brick actor; AdoptLayout only holds it. */
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
	 * Lay footing and post into the layout, set materials, and keep one box per piece (AdoptLayout
	 * refuses a desynced layout). Returns the bed joint, or INDEX_NONE.
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

/** The adopted cross-material joint reads the weakest-link strength, not the bare connection. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAdoptLayoutCarriesPieceMaterialTest,
	"DestructionGame.Acceptance.CrossMaterialBearing.AdoptLayoutCarriesPieceMaterial",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FAdoptLayoutCarriesPieceMaterialTest::RunTest(const FString& Parameters)
{
	using namespace DestructionProfiles;
	using namespace AdoptLayoutMaterialTestSupport;

	TestTrue(
		FString::Printf(TEXT("PRECONDITION: the Unbreakable connection's compressive (%g MPa) must dwarf both "
			"materials, so the MATERIAL crush governs"), Unbreakable.CompressiveStrengthMPa),
		Unbreakable.CompressiveStrengthMPa > 1.0e9);

	TestTrue(
		FString::Printf(TEXT("PRECONDITION: timber compressive must be 29 MPa (C24 mean f_c,0), profile carries %g"),
			Timber.Strength.CompressiveStrengthMPa),
		Timber.Strength.CompressiveStrengthMPa == 29.0);

	TestTrue(
		FString::Printf(TEXT("PRECONDITION: clay brick compressive must be 20 MPa, profile carries %g"),
			ClayBrick.Strength.CompressiveStrengthMPa),
		ClayBrick.Strength.CompressiveStrengthMPa == 20.0);

	const double MaterialCrushMPa = FMath::Min3(
		Unbreakable.CompressiveStrengthMPa,
		Timber.Strength.CompressiveStrengthMPa,
		ClayBrick.Strength.CompressiveStrengthMPa);                                    // 20

	TestTrue(
		FString::Printf(TEXT("PRECONDITION: the weakest-link crush must be the brick's 20 MPa, got %g"),
			MaterialCrushMPa),
		MaterialCrushMPa == 20.0);

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

	// Positive control: the layout's own structure already reads the crush.
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

	TestEqual(TEXT("adoption should carry both pieces"),
		Binding.GetStructure().NumPieces(), 2);
	TestEqual(TEXT("adoption should carry the bed joint"),
		Binding.GetStructure().NumConnections(), 1);

	// Mechanism: the material pointers survive adoption.
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

	// Outcome: 20 MPa with materials carried across, 1e12 with them dropped.
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
