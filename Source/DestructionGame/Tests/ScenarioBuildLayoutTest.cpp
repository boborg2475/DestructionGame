// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "PhysicsEngine/BodyInstance.h"
#include "Tests/BrickWorldTestSupport.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * BuildLayout stands up any layout, not only a running-bond wall; BuildRunningBond is RunningBond
 * followed by BuildLayout. The fixture is a hand-built stepped arm of mixed sizes, so an
 * implementation that re-runs RunningBond or only handles walls fails.
 *
 * Per piece: a live ABrickActor at the box's bounds, carrying the layout's mass and its own
 * FPieceRef, resolving back through the binding. A refused build spends no structure id. Wall
 * specifics are in World.Brick.SpawnedWallMatchesItsLayout. Needs a world, never ticks it.
 */
namespace ScenarioBuildLayoutTestSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	/*
	 * Stepped arm: a grounded brick, a brick one course up stepped half a cell, then a half bat
	 * stepped again. Two sizes, two masses. Standard dimensions restated rather than imported.
	 */
	constexpr double ScenarioBuildLayoutBrickLengthCm = 21.5;
	constexpr double ScenarioBuildLayoutBrickWidthCm = 10.25;
	constexpr double ScenarioBuildLayoutBrickHeightCm = 6.5;
	constexpr double ScenarioBuildLayoutJointCm = 1.0;

	/** Coordinating grid 22.5 x 11.25 x 7.5; the step is half a cell. */
	constexpr double ScenarioBuildLayoutCoursePitchCm =
		ScenarioBuildLayoutBrickHeightCm + ScenarioBuildLayoutJointCm;

	constexpr double ScenarioBuildLayoutStepCm =
		(ScenarioBuildLayoutBrickLengthCm + ScenarioBuildLayoutJointCm) / 2.0;

	/** Half bat: (brick length - one joint) / 2. */
	constexpr double ScenarioBuildLayoutHalfBatLengthCm =
		(ScenarioBuildLayoutBrickLengthCm - ScenarioBuildLayoutJointCm) / 2.0;

	inline FPieceBox ScenarioBuildLayoutBox(double CentreXCm, double CentreZCm, double LengthCm)
	{
		FPieceBox Box;

		Box.CentreCm = FVector(CentreXCm, 0.0, CentreZCm);

		Box.ExtentCm = FVector(
			LengthCm / 2.0,
			ScenarioBuildLayoutBrickWidthCm / 2.0,
			ScenarioBuildLayoutBrickHeightCm / 2.0);

		return Box;
	}

	/**
	 * Lay the arm by hand; joints go through MakeInterface, production's only interface producer.
	 *
	 * @return false if the fixture is broken.
	 */
	inline bool ScenarioBuildLayoutSteppedArm(FBrickLayout& Out)
	{
		Out = FBrickLayout();

		const FPieceBox Boxes[3] =
		{
			ScenarioBuildLayoutBox(0.0, ScenarioBuildLayoutBrickHeightCm / 2.0,
				ScenarioBuildLayoutBrickLengthCm),

			ScenarioBuildLayoutBox(
				ScenarioBuildLayoutStepCm,
				ScenarioBuildLayoutBrickHeightCm / 2.0 + ScenarioBuildLayoutCoursePitchCm,
				ScenarioBuildLayoutBrickLengthCm),

			ScenarioBuildLayoutBox(
				ScenarioBuildLayoutStepCm * 2.0,
				ScenarioBuildLayoutBrickHeightCm / 2.0 + ScenarioBuildLayoutCoursePitchCm * 2.0,
				ScenarioBuildLayoutHalfBatLengthCm),
		};

		for (int32 Piece = 0; Piece < 3; ++Piece)
		{
			const double MassKg = PieceMassKg(Boxes[Piece], ClayBrick.DensityGramsPerCubicCm);

			if (Out.Structure.AddPiece(MassKg, Piece == 0, Boxes[Piece].CentreCm) != Piece)
			{
				return false;
			}

			Out.Boxes.Add(Boxes[Piece]);
		}

		for (int32 Lower = 0; Lower + 1 < 3; ++Lower)
		{
			FConnection Joint;

			if (!MakeInterface(
					Lower, Boxes[Lower], Lower + 1, Boxes[Lower + 1],
					ScenarioBuildLayoutJointCm, GeneralPurposeMortar, Joint))
			{
				return false;
			}

			if (Out.Structure.AddConnection(Joint) == INDEX_NONE)
			{
				return false;
			}
		}

		return true;
	}

	/** Mass from geometry, derived here rather than imported. */
	inline double ScenarioBuildLayoutMassKg(const FPieceBox& Box)
	{
		const double VolumeCubicCm =
			(Box.ExtentCm.X * 2.0) * (Box.ExtentCm.Y * 2.0) * (Box.ExtentCm.Z * 2.0);

		return VolumeCubicCm * ClayBrick.DensityGramsPerCubicCm / 1000.0;
	}

	/** Check a built structure: one brick per piece, correctly placed, weighted, and identified. */
	inline void ScenarioBuildLayoutCheckBuilt(
		FAutomationTestBase& Test,
		const TCHAR* Label,
		FStructureBinding& Binding,
		int32 StructureId,
		const FBrickLayout& Layout)
	{
		Test.TestTrue(
			*FString::Printf(
				TEXT("%s: the binding must carry the id it was handed out under (%d); it carries %d"),
				Label, StructureId, Binding.StructureId),
			Binding.StructureId == StructureId);

		Test.TestTrue(
			*FString::Printf(
				TEXT("%s: it must span the layout's %d piece handles and %d joints; it spans %d and %d"),
				Label, Layout.Structure.NumPieces(), Layout.Structure.NumConnections(),
				Binding.NumPieces(), Binding.GetStructure().NumConnections()),
			Binding.NumPieces() == Layout.Structure.NumPieces()
				&& Binding.GetStructure().NumConnections() == Layout.Structure.NumConnections());

		for (int32 Piece = 0; Piece < Layout.Boxes.Num() && Piece < Binding.NumPieces(); ++Piece)
		{
			const DestructionLayout::FPieceBox& Box = Layout.Boxes[Piece];

			ABrickActor* const Brick = Cast<ABrickActor>(Binding.GetActor(Piece));

			if (!IsValid(Brick))
			{
				Test.AddError(FString::Printf(
					TEXT("%s: piece %d must be bound to a live ABrickActor, it is bound to %s"),
					Label, Piece, *GetNameSafe(Binding.GetActor(Piece))));

				continue;
			}

			// Bounds, not actor location: SM_Cube's pivot is a corner (BrickWorldTestSupport.h).
			const FBox BoundsCm = Brick->GetComponentsBoundingBox(/*bNonColliding*/ true);

			Test.TestTrue(
				*FString::Printf(
					TEXT("%s: piece %d's brick must occupy its own box, centre (%g, %g, %g) and ")
					TEXT("half-extent (%g, %g, %g); its bounds are centred (%g, %g, %g) at ")
					TEXT("(%g, %g, %g)"),
					Label, Piece,
					Box.CentreCm.X, Box.CentreCm.Y, Box.CentreCm.Z,
					Box.ExtentCm.X, Box.ExtentCm.Y, Box.ExtentCm.Z,
					BoundsCm.GetCenter().X, BoundsCm.GetCenter().Y, BoundsCm.GetCenter().Z,
					BoundsCm.GetExtent().X, BoundsCm.GetExtent().Y, BoundsCm.GetExtent().Z),
				BoundsCm.GetCenter().Equals(Box.CentreCm, BrickWorldTestSupport::BoundsToleranceCm)
					&& BoundsCm.GetExtent().Equals(
						Box.ExtentCm, BrickWorldTestSupport::BoundsToleranceCm));

			// Read the mass override, since a kinematic body may report GetMass as 0.
			const double ExpectedMassKg = ScenarioBuildLayoutMassKg(Box);

			const FBodyInstance* const Body = Brick->GetMesh() != nullptr
				? Brick->GetMesh()->GetBodyInstance()
				: nullptr;

			if (Body == nullptr)
			{
				Test.AddError(FString::Printf(
					TEXT("%s: piece %d's brick must have a body instance to weigh"), Label, Piece));
			}
			else
			{
				Test.TestTrue(
					*FString::Printf(
						TEXT("%s: piece %d must weigh the %.9g kg its box implies rather than ")
						TEXT("whatever its mesh volume does; its override is %s%.9g"),
						Label, Piece, ExpectedMassKg,
						Body->bOverrideMass ? TEXT("") : TEXT("NOT OVERRIDDEN, "),
						static_cast<double>(Body->GetMassOverride())),
					Body->bOverrideMass
						&& FMath::Abs(
							static_cast<double>(Body->GetMassOverride()) - ExpectedMassKg) < 1.0e-4);
			}

			// There is no actor-to-handle map, so a wrong ref makes the brick unclickable.
			Test.TestTrue(
				*FString::Printf(
					TEXT("%s: piece %d's brick must carry ref {%d, %d}; it carries {%d, %d}"),
					Label, Piece, StructureId, Piece,
					Brick->GetPieceRef().StructureId, Brick->GetPieceRef().PieceIndex),
				Brick->GetPieceRef().StructureId == StructureId
					&& Brick->GetPieceRef().PieceIndex == Piece);

			Test.TestTrue(
				*FString::Printf(
					TEXT("%s: piece %d's own ref must resolve back to handle %d; it resolved to %d"),
					Label, Piece, Piece, Binding.ResolvePiece(Brick->GetPieceRef())),
				Binding.ResolvePiece(Brick->GetPieceRef()) == Piece);
		}
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FScenarioBuildLayoutTest,
	"DestructionGame.World.Subsystem.BuildLayout",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FScenarioBuildLayoutTest::RunTest(const FString& Parameters)
{
	using namespace ScenarioBuildLayoutTestSupport;
	using namespace BrickWorldTestSupport;
	using namespace DestructionLayout;

	FBrickLayout Arm;

	if (!ScenarioBuildLayoutSteppedArm(Arm))
	{
		AddError(TEXT("fixture: the three-piece stepped arm must lay"));
		return true;
	}

	TestTrue(
		*FString::Printf(
			TEXT("fixture: the arm must be 3 pieces and 2 bed joints, and NOT a running bond; ")
			TEXT("it is %d and %d"),
			Arm.Structure.NumPieces(), Arm.Structure.NumConnections()),
		Arm.Structure.NumPieces() == 3 && Arm.Structure.NumConnections() == 2);

	TestTrue(
		TEXT("fixture: the arm must be MIXED SIZE — a spawner that handed every brick one box ")
		TEXT("would otherwise pass"),
		Arm.Boxes[2].ExtentCm.X != Arm.Boxes[0].ExtentCm.X);

	FBrickTestWorld TestWorld;

	if (!TestWorld.Begin(*this))
	{
		return true;
	}

	// One: a layout no production producer emits.

	const int32 ArmId = TestWorld.Subsystem->BuildLayout(Arm);

	TestTrue(
		*FString::Printf(
			TEXT("BuildLayout must build a structure from a layout that is not a running bond; ")
			TEXT("it returned %d"),
			ArmId),
		ArmId != INDEX_NONE);

	if (FStructureBinding* const ArmBinding = TestWorld.Subsystem->Find(ArmId))
	{
		ScenarioBuildLayoutCheckBuilt(*this, TEXT("the stepped arm"), *ArmBinding, ArmId, Arm);
	}
	else
	{
		AddError(FString::Printf(
			TEXT("the subsystem must hold the structure BuildLayout just built (id %d) — a wall ")
			TEXT("nothing names is a wall nothing can click"),
			ArmId));
	}

	// Two: a second build is a second structure.

	const int32 SecondArmId = TestWorld.Subsystem->BuildLayout(Arm);

	TestTrue(
		*FString::Printf(
			TEXT("a second BuildLayout must be a SECOND structure with its own id; the first was ")
			TEXT("%d and the second is %d"),
			ArmId, SecondArmId),
		SecondArmId != INDEX_NONE && SecondArmId != ArmId
			&& TestWorld.Subsystem->Find(SecondArmId) != nullptr
			&& TestWorld.Subsystem->Find(ArmId) != nullptr);

	// Three: a refused build writes nothing and spends no id.

	{
		// An empty layout and one with more boxes than pieces (RunningBond can produce one).
		FBrickLayout Nothing;

		// Copied out first: TArray::Add asserts when adding an element of the same array.
		const FPieceBox SpareBox = Arm.Boxes[0];

		FBrickLayout Desynced = Arm;
		Desynced.Boxes.Add(SpareBox);

		const int32 RefusedEmpty = TestWorld.Subsystem->BuildLayout(Nothing);
		const int32 RefusedDesynced = TestWorld.Subsystem->BuildLayout(Desynced);

		TestTrue(
			*FString::Printf(
				TEXT("an EMPTY layout must build nothing; BuildLayout returned %d"), RefusedEmpty),
			RefusedEmpty == INDEX_NONE);

		TestTrue(
			*FString::Printf(
				TEXT("a layout with %d boxes for %d pieces must build nothing; BuildLayout ")
				TEXT("returned %d"),
				Desynced.Boxes.Num(), Desynced.Structure.NumPieces(), RefusedDesynced),
			RefusedDesynced == INDEX_NONE);

		TestTrue(
			*FString::Printf(
				TEXT("a refusal must register nothing: Find(%d) and Find(%d) must both be null"),
				RefusedEmpty, RefusedDesynced),
			TestWorld.Subsystem->Find(RefusedEmpty) == nullptr
				&& TestWorld.Subsystem->Find(RefusedDesynced) == nullptr);

		// The next id must follow the second success, not the refusals.
		const int32 NextId = TestWorld.Subsystem->BuildLayout(Arm);

		TestTrue(
			*FString::Printf(
				TEXT("a refused build must spend no structure id: after ids %d and %d succeeded ")
				TEXT("and two builds were refused, the next id must be %d; it is %d"),
				ArmId, SecondArmId, SecondArmId + 1, NextId),
			NextId == SecondArmId + 1);
	}

	// Four: BuildRunningBond equals BuildLayout(RunningBond(Spec)).

	{
		const FRunningBondSpec Spec = WallSpec();

		FBrickLayout Laid;

		if (!RunningBond(Spec, Laid))
		{
			AddError(TEXT("fixture: the shared 2 x 3 flush wall spec must lay"));

			TestWorld.End();
			return true;
		}

		const int32 SpecId = TestWorld.Subsystem->BuildRunningBond(Spec);
		const int32 LayoutId = TestWorld.Subsystem->BuildLayout(Laid);

		FStructureBinding* const FromSpec = TestWorld.Subsystem->Find(SpecId);
		FStructureBinding* const FromLayout = TestWorld.Subsystem->Find(LayoutId);

		if (FromSpec == nullptr || FromLayout == nullptr)
		{
			AddError(FString::Printf(
				TEXT("BuildRunningBond (id %d) and BuildLayout on the wall it lays (id %d) must ")
				TEXT("both build"),
				SpecId, LayoutId));

			TestWorld.End();
			return true;
		}

		// Both checked against the same layout, making this an equivalence.
		ScenarioBuildLayoutCheckBuilt(
			*this, TEXT("BuildRunningBond's own wall"), *FromSpec, SpecId, Laid);

		ScenarioBuildLayoutCheckBuilt(
			*this, TEXT("BuildLayout on the same laid wall"), *FromLayout, LayoutId, Laid);

		TestTrue(
			*FString::Printf(
				TEXT("BuildRunningBond must still lay the wall it lays today: %d pieces and %d ")
				TEXT("joints, the same as BuildLayout on the same spec's layout (%d and %d)"),
				FromSpec->NumPieces(), FromSpec->GetStructure().NumConnections(),
				FromLayout->NumPieces(), FromLayout->GetStructure().NumConnections()),
			FromSpec->NumPieces() == WallPieceCount
				&& FromSpec->NumPieces() == FromLayout->NumPieces()
				&& FromSpec->GetStructure().NumConnections()
					== FromLayout->GetStructure().NumConnections());
	}

	TestWorld.End();

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
