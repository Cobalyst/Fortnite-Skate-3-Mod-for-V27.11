#include "DropperData.h"

#include <cstring>
#include <fstream>
#include <sstream>

namespace DropperData
{
    namespace
    {
        struct Reader
        {
            const std::vector<char>& D;
            size_t P = 0;
            bool Bad = false;
            template <typename T> T Get()
            {
                T V{};
                if (P + sizeof(T) > D.size()) { Bad = true; return V; }
                memcpy(&V, D.data() + P, sizeof(T));
                P += sizeof(T);
                return V;
            }
            void Bytes(void* Out, size_t N)
            {
                if (P + N > D.size()) { Bad = true; return; }
                memcpy(Out, D.data() + P, N);
                P += N;
            }
            std::string Str16()
            {
                const uint16_t N = Get<uint16_t>();
                if (P + N > D.size()) { Bad = true; return {}; }
                std::string S(D.data() + P, N);
                P += N;
                return S;
            }
        };
    }

    bool ReadSkob(const std::string& Path, Object& Out, std::string& Error)
    {
        std::ifstream In(Path, std::ios::binary);
        if (!In) { Error = "cannot open " + Path; return false; }
        std::vector<char> D((std::istreambuf_iterator<char>(In)), std::istreambuf_iterator<char>());
        Reader R{ D };
        char Magic[4];
        R.Bytes(Magic, 4);
        const uint32_t Version = R.Get<uint32_t>();
        if (R.Bad || memcmp(Magic, "SKOB", 4) != 0 || Version < 1 || Version > 2) { Error = "not a .skob v1/v2: " + Path; return false; }
        for (float& V : Out.Min) V = R.Get<float>();
        for (float& V : Out.Max) V = R.Get<float>();
        Out.Materials.resize(R.Get<uint32_t>());
        for (Material& M : Out.Materials)
        {
            M.Flags = R.Get<uint32_t>();
            M.Diffuse = R.Str16();
            M.Normal = R.Str16();
            M.Specular = R.Str16();
        }
        Out.Meshes.resize(R.Get<uint32_t>());
        for (Mesh& M : Out.Meshes)
        {
            M.Material = R.Get<uint32_t>();
            const uint32_t Nv = R.Get<uint32_t>(), Ni = R.Get<uint32_t>();
            if (R.Bad || Nv > 1000000 || Ni > 3000000) { Error = "bad mesh in " + Path; return false; }
            M.Vertices.resize(size_t(Nv) * 13);
            R.Bytes(M.Vertices.data(), M.Vertices.size() * 4);
            M.Indices.resize(Ni);
            R.Bytes(M.Indices.data(), M.Indices.size() * 4);
        }
        Out.Collision.resize(R.Get<uint32_t>());
        for (CollisionTriangle& T : Out.Collision)
        {
            R.Bytes(T.V, sizeof(T.V));
            T.Surface = R.Get<uint16_t>();
            R.Bytes(T.Edges, 3);
            T.UnitFlags = R.Get<uint8_t>();
        }
        Out.Volumes.resize(R.Get<uint32_t>());
        for (Volume& V : Out.Volumes)
        {
            V.Type = R.Get<uint32_t>();
            R.Bytes(V.Rot, sizeof(V.Rot));
            R.Bytes(V.Pos, sizeof(V.Pos));
            R.Bytes(V.Params, sizeof(V.Params));
            V.Surface = R.Get<uint32_t>();
        }
        Out.RigidBody.resize(R.Get<uint32_t>());
        R.Bytes(Out.RigidBody.data(), Out.RigidBody.size());
        Out.RigidBodyShape.resize(R.Get<uint32_t>());
        R.Bytes(Out.RigidBodyShape.data(), Out.RigidBodyShape.size());
        if (Version >= 2)
        {
            Out.Grinds.resize(R.Get<uint32_t>());
            for (GrindSpline& G : Out.Grinds)
            {
                G.Flags = R.Get<uint32_t>();
                G.Surface = R.Get<uint32_t>();
                for (float& V : G.Normal) V = R.Get<float>();
                const uint32_t N = R.Get<uint32_t>();
                if (R.Bad || N > 100000) { Error = "bad grind spline in " + Path; return false; }
                G.Segments.resize(size_t(N) * 12);
                R.Bytes(G.Segments.data(), G.Segments.size() * 4);
            }
        }
        if (R.Bad) { Error = "truncated " + Path; return false; }
        return true;
    }

    bool Library::Load(const std::string& Root, std::string& Error)
    {
        Dir = Root;
        std::ifstream In(Root + "\\objects.txt");
        if (!In) { Error = "cannot open " + Root + "\\objects.txt"; return false; }
        std::string Line;
        while (std::getline(In, Line))
        {
            if (Line.empty() || Line[0] == '#') continue;
            if (Line.back() == '\r') Line.pop_back();
            std::vector<std::string> F;
            std::stringstream S(Line);
            for (std::string Field; std::getline(S, Field, '\t');) F.push_back(Field);
            if (F.size() < 6) continue;
            CatalogEntry E{ F[0], F[1], F[2], F[3], F[4], {} };
            std::stringstream C(F[5]);
            for (std::string Kv; std::getline(C, Kv, ';');)
            {
                const size_t Eq = Kv.find('=');
                if (Eq != std::string::npos) E.Caps[Kv.substr(0, Eq)] = Kv.substr(Eq + 1);
            }
            ById[E.Id] = Entries.size();
            Entries.push_back(std::move(E));
        }
        if (Entries.empty()) { Error = "empty catalog"; return false; }
        return true;
    }

    const CatalogEntry* Library::Find(const std::string& Id) const
    {
        auto It = ById.find(Id);
        return It == ById.end() ? nullptr : &Entries[It->second];
    }

    std::shared_ptr<const Object> Library::Get(const std::string& Id, std::string* Error)
    {
        auto It = Loaded.find(Id);
        if (It != Loaded.end()) return It->second;
        auto O = std::make_shared<Object>();
        std::string E;
        if (!ReadSkob(Dir + "\\objects\\" + Id + ".skob", *O, E))
        {
            if (Error) *Error = E;
            Loaded[Id] = nullptr;
            return nullptr;
        }
        Loaded[Id] = O;
        return O;
    }
}
