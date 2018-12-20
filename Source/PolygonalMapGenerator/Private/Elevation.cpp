/*
* From http://www.redblobgames.com/maps/mapgen2/
* Original work copyright 2017 Red Blob Games <redblobgames@gmail.com>
* Unreal Engine 4 implementation copyright 2018 Jay Stevens <jaystevens42@gmail.com>
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
* http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*/
#include "Elevation.h"

TArray<FTriangleIndex> UElevation::find_coasts_t(UTriangleDualMesh* Mesh, const TArray<bool>& r_ocean) const
{
	TSet<FTriangleIndex> coasts_t;
	for (int s = 0; s < Mesh->NumSides; s++)
	{
		FPointIndex r0 = Mesh->s_begin_r(s);
		FPointIndex r1 = Mesh->s_end_r(s);
		FTriangleIndex t = Mesh->s_inner_t(s);
		if (r_ocean[r0] && !r_ocean[r1])
		{
			// It might seem that we also need to check !r_ocean[r0] && r_ocean[r1]
			// and it might seem that we have to add both t and its opposite but
			// each t vertex shows up in *four* directed sides, so we only have to test
			// one fourth of those conditions to get the vertex in the list once.
			coasts_t.Add(t);
		}
	}
	return coasts_t.Array();
}

bool UElevation::t_ocean(FTriangleIndex t, UTriangleDualMesh* Mesh, const TArray<bool>& r_ocean) const
{
	return r_ocean[Mesh->s_begin_r(UDelaunayHelper::TriangleIndexToEdge(t))];
}

bool UElevation::r_lake(FPointIndex r, const TArray<bool>& r_water, const TArray<bool>& r_ocean) const
{
	return r_water[r] && !r_ocean[r];
}

bool UElevation::s_lake(FSideIndex s, UTriangleDualMesh* Mesh, const TArray<bool>& r_water, const TArray<bool>& r_ocean) const
{
	return r_lake(Mesh->s_begin_r(s), r_water, r_ocean) || r_lake(Mesh->s_end_r(s), r_water, r_ocean);
}

void UElevation::assign_t_elevation(TArray<float>& t_elevation, TArray<float>& t_coastdistance, TArray<FSideIndex>& t_downslope_s, UTriangleDualMesh* Mesh, const TArray<bool>& r_ocean, const TArray<bool>& r_water, FRandomStream& DrainageRng) const
{
	// TODO: this messes up lakes, as they will no longer all be at the same elevation
	t_coastdistance.Empty(Mesh->NumTriangles);
	t_coastdistance.SetNumZeroed(Mesh->NumTriangles);
	for (int i = 0; i < t_coastdistance.Num(); i++)
	{
		t_coastdistance[i] = -1.0f;
	}
	t_downslope_s.Empty(Mesh->NumTriangles);
	t_downslope_s.SetNumZeroed(Mesh->NumTriangles);
	for (int i = 0; i < t_downslope_s.Num(); i++)
	{
		t_downslope_s[i] = FSideIndex();
	}
	t_elevation.Empty(Mesh->NumTriangles);
	t_elevation.SetNumZeroed(Mesh->NumTriangles);

	TArray<FSideIndex> out_s;
	TArray<FTriangleIndex> queue_t = find_coasts_t(Mesh, r_ocean);
	for (int t = 0; t < queue_t.Num(); t++)
	{
		t_coastdistance[t] = 0;
	}
	float minDistance = 1.0f;
	float maxDistance = 1.0f;

	while (queue_t.Num() > 0)
	{
		// Get the next triangle and pop it from the queue
		FTriangleIndex current_t = queue_t[0];
		queue_t.RemoveAt(0);
		// Find all sides of the current triangle
		out_s = Mesh->t_circulate_s(current_t);

		// Iterate over each side of the triangle, starting from a random offset
		int32 iOffset = DrainageRng.RandRange(0, out_s.Num() - 1);
		for (int i = 0; i < out_s.Num(); i++)
		{
			// Get the index of the side we're working on
			FSideIndex s = out_s[(i + iOffset) % out_s.Num()];
			// Check to see if this side is a lake
			bool lake = s_lake(s, Mesh, r_water, r_ocean);
			FTriangleIndex neighbor_t = Mesh->s_outer_t(s);
			float newDistance = (lake ? 0.0f : 1.0f) + t_coastdistance[current_t];
			if (t_coastdistance[neighbor_t] == -1.0f || newDistance < t_coastdistance[neighbor_t])
			{
				t_downslope_s[neighbor_t] = Mesh->s_opposite_s(s);
				t_coastdistance[neighbor_t] = newDistance;
				if (t_ocean(neighbor_t, Mesh, r_ocean) && newDistance > minDistance) { minDistance = newDistance; }
				if (!t_ocean(neighbor_t, Mesh, r_ocean) && newDistance > maxDistance) { maxDistance = newDistance; }
				if (lake)
				{
					queue_t.EmplaceAt(0);
					queue_t[0] = neighbor_t;
				}
				else
				{
					queue_t.Add(neighbor_t);
				}
			}
		}
	}

	for (int t = 0; t < t_coastdistance.Num(); t++)
	{
		float d = t_coastdistance[t];
		t_elevation[t] = t_ocean(t, Mesh, r_ocean) ? (-d / minDistance) : (d / maxDistance);
	}
}

void UElevation::redistribute_t_elevation(TArray<float>& t_elevation, UTriangleDualMesh* Mesh) const
{
	// SCALE_FACTOR increases the mountain area. At 1.0 the maximum
	// elevation barely shows up on the map, so we set it to 1.1.
	const float SCALE_FACTOR = 1.1f;

	TArray<FTriangleIndex> nonocean_t;
	for (FTriangleIndex t = 0; t < Mesh->NumSolidTriangles; t++)
	{
		if (t_elevation[t] > 0.0)
		{
			nonocean_t.Add(t);
		}
	}

	nonocean_t.Sort([t_elevation](const FTriangleIndex& A, const FTriangleIndex& B)
	{
		return t_elevation[A] > t_elevation[B];
	});

	if (nonocean_t.Num() > 0)
	{
		UE_LOG(LogMapGen, Log, TEXT("Sorted non-ocean bottom value: %f. Sorted non-ocean top value: %f"), t_elevation[nonocean_t[0]], t_elevation[nonocean_t[nonocean_t.Num() - 1]]);
	}

	for (int i = 0; i < nonocean_t.Num(); i++)
	{
		// Let y(x) be the total area that we want at elevation <= x.
		// We want the higher elevations to occur less than lower
		// ones, and set the area to be y(x) = 1 - (1-x)^2.
		float y = i / (nonocean_t.Num() - 1.0f);
		// Now we have to solve for x, given the known y.
		//  *  y = 1 - (1-x)^2
		//  *  y = 1 - (1 - 2x + x^2)
		//  *  y = 2x - x^2
		//  *  x^2 - 2x + y = 0
		// From this we can use the quadratic equation to get:
		float x = FMath::Sqrt(SCALE_FACTOR) - FMath::Sqrt(SCALE_FACTOR*(1 - y));
		if (x > 1.0)
		{
			x = 1.0;
		}
		t_elevation[nonocean_t[i]] = x;
	}
}

void UElevation::assign_r_elevation(TArray<float>& r_elevation, UTriangleDualMesh* Mesh, const TArray<float>& t_elevation, const TArray<bool>& r_ocean) const
{
	const float max_ocean_elevation = -0.01;

	r_elevation.Empty(Mesh->NumTriangles);
	r_elevation.SetNumZeroed(Mesh->NumTriangles);

	TArray<FTriangleIndex> out_t;
	for (FPointIndex r = 0; r < Mesh->NumRegions; r++)
	{
		out_t = Mesh->r_circulate_t(r);
		float elevation = 0.0f;
		for (FTriangleIndex t : out_t)
		{
			elevation += t_elevation[t];
		}

		r_elevation[r] = elevation / out_t.Num();
		if (r_ocean[r] && r_elevation[r] > max_ocean_elevation)
		{
			r_elevation[r] = max_ocean_elevation;
		}
	}
}
