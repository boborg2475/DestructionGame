// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "PhysicsEngine/BodyInstance.h"
#include "Tests/BrickWorldTestSupport.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * THE SUBSYSTEM CAN STAND UP ANY LAYOUT, NOT ONLY A RUNNING-BOND WALL.
 *
 * =====================================================================================
 * WHY THE GENERAL DOOR HAS TO EXIST
 * =====================================================================================
 *
 * `BuildRunningBond` takes an `FRunningBondSpec`, so the ONLY structure this subsystem can put
 * into a world is a running-bond wall. The corbel family, the staircase and every acceptance
 * wall are laid by other producers, and `Tests/CorbelScreenshotTest.cpp` says so in its own
 * header — it hand-rolls a spawn loop because there was no other way in. A second spawn loop is
 * a second answer to where a brick goes, how much it weighs and which ref it carries, and this
 * project has already paid twice for two answers to one question.
 *
 * So the seam is the LAYOUT, which is what every producer already emits: `BuildRunningBond`
 * becomes `RunningBond` then `BuildLayout`, and nothing else changes.
 *
 * =====================================================================================
 * WHAT IS ASSERTED, AND WHAT IS DELIBERATELY NOT
 * =====================================================================================
 *
 * The layout under test is HAND-BUILT and is deliberately NOT a running bond: three boxes in a
 * stepped arm, two of them mixed sizes, one grounded. A `BuildLayout` implemented by sniffing
 * the layout and calling `RunningBond` again would fail on it, and so would one that quietly
 * only ever handles wall-shaped input.
 *
 * Per piece: one live `ABrickActor`, standing where the box says and the size the box says, told
 * its own `FPieceRef`, resolving back through the binding, and carrying the mass the layout gave
 * it. The mass matters as much as the position — a general door that forgot it would leave every
 * brick weighing whatever its mesh volume implies, which is emphatically not what the solver is
 * using, and nothing on screen would look wrong.
 *
 * A REFUSED ADOPTION SPENDS NO ID, and that is a new claim: nothing in the suite pins it today
 * for `BuildRunningBond` either. It is asserted the only way it can be — by refusing a build and
 * then checking that the NEXT successful build gets the id the refused one would have had. An id
 * spent on nothing is a hole in the numbering, and every brick carries its structure id.
 *
 * NOT ASSERTED HERE: everything `World.Brick.SpawnedWallMatchesItsLayout` already pins about a
 * running-bond wall — mobility, tick, the trace, the two brick masses. This is about the door,
 * not about the wall behind it.
 *
 * NEEDS A TICKING WORLD: it needs a WORLD, to spawn actors into. It never ticks one; nothing
 * here is about anything moving.
 */
namespace ScenarioBuildLayoutTestSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	/*
	 * --- a structure that is deliberately not a wall -------------------------------------
	 *
	 * A THREE-PIECE STEPPED ARM. Piece 0 is a full brick on the ground; piece 1 is a full brick
	 * one course up, stepped half a cell out; piece 2 is a HALF BAT on the course above that,
	 * stepped again. Two sizes, two masses, one grounded piece and two bed joints — everything a
	 * spawner could get uniformly wrong is different between at least two pieces.
	 *
	 * THE NUMBERS ARE THE STANDARD BRICK AND THE STANDARD JOINT, restated here rather than
	 * imported, so a fixture that drifted from the coordinating grid says so.
	 */
	constexpr double ScenarioBuildLayoutBrickLengthCm = 21.5;
	constexpr double ScenarioBuildLayoutBrickWidthCm = 10.25;
	constexpr double ScenarioBuildLayoutBrickHeightCm = 6.5;
	constexpr double ScenarioBuildLayoutJointCm = 1.0;

	/** The coordinating grid: 22.5 x 11.25 x 7.5, and the step is half a cell along a course. */
	constexpr double ScenarioBuildLayoutCoursePitchCm =
		ScenarioBuildLayoutBrickHeightCm + ScenarioBuildLayoutJointCm;

	constexpr double ScenarioBuildLayoutStepCm =
		(ScenarioBuildLayoutBrickLengthCm + ScenarioBuildLayoutJointCm) / 2.0;

	/** A half bat is what is left of a brick once one joint is taken out of its length. */
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
	 * The arm, laid by hand: boxes, masses, grounding and the two bed joints between them.
	 *
	 * MakeInterface IS USED FOR THE JOINTS AND NOTHING ELSE IS. It is production's only route to
	 * an interface area and a normal, and writing one here instead would be a second producer of
	 * exactly the quantity the whole solver consumes. What this fixture supplies is the PAIRS,
	 * which is the half `RunningBond` supplies for a wall.
	 *
	 * @return false if the arm could not be laid, which is a broken fixture rather than a result.
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

	/** Mass from geometry, derived here rather than imported — see BrickWorldTestSupport.h. */
	inline double ScenarioBuildLayoutMassKg(const FPieceBox& Box)
	{
		const double VolumeCubicCm =
			(Box.ExtentCm.X * 2.0) * (Box.ExtentCm.Y * 2.0) * (Box.ExtentCm.Z * 2.0);

		return VolumeCubicCm * ClayBrick.DensityGramsPerCubicCm / 1000.0;
	}

	/**
	 * Every claim about one built structure: one brick per piece, in the right place, at the
	 * right weight, carrying the right identity.
	 *
	 * WRITTEN ONCE BECAUSE IT IS ASSERTED TWICE — of the hand-built arm, and of the wall
	 * `BuildRunningBond` produces, which must be the same structure `BuildLayout` produces from
	 * the same layout. Two copies of it would be two standards for what "built" means.
	 */
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

			/*
			 * ON BOUNDS, NOT ON THE ACTOR'S LOCATION. SM_Cube's pivot is a CORNER, so an actor
			 * placed at a box's centre puts the mesh a half-size out on all three axes while
			 * GetActorLocation agrees with the layout perfectly — see BrickWorldTestSupport.h.
			 */
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

			/*
			 * THE MASS THE LAYOUT ALREADY DERIVED, READ BACK AS THE CONFIGURED OVERRIDE. A
			 * kinematic body may report no mass to the solver at all, so GetMass here risks
			 * being 0 == 0; the override is what SetMassOverrideInKg wrote and is readable
			 * while kinematic. Same reasoning as Tests/BrickActorTest.cpp.
			 */
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

			/*
			 * AND IT KNOWS WHO IT IS. There is no actor-to-handle map anywhere in this subsystem
			 * — the answer travels on the brick — so a brick told nothing, or told somebody
			 * else's index, is a brick the player can see and cannot click.
			 */
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

	/* --- ONE: a layout nothing in production produces, stood up in the world -------------- */

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

	/* --- TWO: a second build is a second structure, not the same one over again ----------- */

	const int32 SecondArmId = TestWorld.Subsystem->BuildLayout(Arm);

	TestTrue(
		*FString::Printf(
			TEXT("a second BuildLayout must be a SECOND structure with its own id; the first was ")
			TEXT("%d and the second is %d"),
			ArmId, SecondArmId),
		SecondArmId != INDEX_NONE && SecondArmId != ArmId
			&& TestWorld.Subsystem->Find(SecondArmId) != nullptr
			&& TestWorld.Subsystem->Find(ArmId) != nullptr);

	/* --- THREE: a refused adoption writes nothing and spends no id ------------------------ */

	{
		/*
		 * AN EMPTY LAYOUT AND A DESYNCED ONE, which are the two things AdoptLayout refuses at
		 * the door. The desynced one is the case that matters: RunningBond can genuinely produce
		 * a layout with one more box than pieces, and a build that adopted it index-by-index
		 * would launder a known-bad input into the one type whose whole contract is that its two
		 * arrays cannot desync.
		 */
		FBrickLayout Nothing;

		/*
		 * THE SPARE BOX IS COPIED OUT FIRST. TArray::Add asserts on an element that already lives
		 * in the array being grown — the reallocation would leave the reference dangling — so
		 * `Boxes.Add(Boxes[0])` is a hard crash rather than a duplicate. (It really is: this was
		 * written that way first.)
		 */
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

		/*
		 * AND THE NUMBERING IS UNTOUCHED, which is the claim the two rows above cannot make on
		 * their own. Two builds have succeeded and two have been refused, so the next id must be
		 * the one that follows the second SUCCESS — a refusal that spent an id leaves a hole in
		 * a numbering every brick in the world carries a copy of.
		 */
		const int32 NextId = TestWorld.Subsystem->BuildLayout(Arm);

		TestTrue(
			*FString::Printf(
				TEXT("a refused build must spend no structure id: after ids %d and %d succeeded ")
				TEXT("and two builds were refused, the next id must be %d; it is %d"),
				ArmId, SecondArmId, SecondArmId + 1, NextId),
			NextId == SecondArmId + 1);
	}

	/* --- FOUR: BuildRunningBond is this door with RunningBond in front of it -------------- */

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

		/*
		 * BOTH ARE CHECKED AGAINST THE SAME LAYOUT, which is what makes this an EQUIVALENCE
		 * rather than two separate characterisations: `BuildRunningBond(Spec)` must produce
		 * exactly what `BuildLayout(RunningBond(Spec))` produces, brick for brick.
		 */
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
