// protohints lists what protobuf alone does not tell about the Kubernetes
// API types, one "message.field" a line, for yapi's protobuf decoder:
//
//   - pointer-fields.txt: scalar fields behind a pointer (*string, *int32,
//     *bool, ...), where "", 0 and false are values: protobuf writes every
//     other scalar even when it is zero, where JSON leaves it out;
//
//   - inline-fields.txt: embedded structs JSON inlines (Volume's
//     VolumeSource), which protobuf keeps as a field of their own;
//
//   - renamed-fields.txt: "message.field jsonName" where the protobuf name
//     is not the JSON one (admissionregistration's "Webhooks").
//
//     go run ./hack/protohints <proto-dir> \
//     $(go list -m -f '{{.Dir}}' k8s.io/api)=k8s.io.api \
//     $(go list -m -f '{{.Dir}}' k8s.io/apimachinery)=k8s.io.apimachinery \
//     $(go list -m -f '{{.Dir}}' k8s.io/apiextensions-apiserver)=k8s.io.apiextensions_apiserver \
//     $(go list -m -f '{{.Dir}}' k8s.io/kube-aggregator)=k8s.io.kube_aggregator
//
// The staging directories of a Kubernetes checkout serve as well, for
// modules this one does not depend on.
package main

import (
	"go/ast"
	"go/parser"
	"go/token"
	"os"
	"path/filepath"
	"reflect"
	"sort"
	"strings"
)

var scalar = map[string]bool{"string": true, "int32": true, "int64": true, "bool": true, "float64": true, "uint32": true, "uint64": true}

func protoName(tag *ast.BasicLit) string {
	if tag == nil {
		return ""
	}
	st := reflect.StructTag(strings.Trim(tag.Value, "`"))
	for _, p := range strings.Split(st.Get("protobuf"), ",") {
		if strings.HasPrefix(p, "name=") {
			return strings.TrimPrefix(p, "name=")
		}
	}
	return ""
}

func main() {
	out := os.Args[1]
	var pointers, inlines, renames []string
	for _, arg := range os.Args[2:] {
		root, prefix, _ := strings.Cut(arg, "=")
		_ = filepath.WalkDir(root, func(path string, d os.DirEntry, err error) error {
			if err != nil || d.IsDir() || !strings.HasSuffix(path, ".go") || strings.HasSuffix(path, "_test.go") ||
				strings.Contains(path, "/vendor/") || strings.HasPrefix(filepath.Base(path), "zz_generated") ||
				strings.HasSuffix(path, ".pb.go") {
				return nil
			}
			rel, _ := filepath.Rel(root, filepath.Dir(path))
			pkg := prefix + "." + strings.ReplaceAll(rel, string(filepath.Separator), ".")
			f, err := parser.ParseFile(token.NewFileSet(), path, nil, 0)
			if err != nil {
				return nil
			}
			ast.Inspect(f, func(n ast.Node) bool {
				ts, ok := n.(*ast.TypeSpec)
				if !ok {
					return true
				}
				st, ok := ts.Type.(*ast.StructType)
				if !ok {
					return true
				}
				for _, field := range st.Fields.List {
					name := protoName(field.Tag)
					if name == "" {
						continue
					}
					// An embedded struct without a JSON name of its own is inlined,
					// whether the tag says ",inline" or, lately, just "".
					jsonName, _, _ := strings.Cut(reflect.StructTag(strings.Trim(field.Tag.Value, "`")).Get("json"), ",")
					if len(field.Names) == 0 && jsonName == "" {
						inlines = append(inlines, pkg+"."+ts.Name.Name+"."+name)
						continue
					}
					if jsonName != "" && jsonName != "-" && jsonName != name {
						renames = append(renames, pkg+"."+ts.Name.Name+"."+name+" "+jsonName)
					}
					if star, ok := field.Type.(*ast.StarExpr); ok {
						if id, ok := star.X.(*ast.Ident); ok && scalar[id.Name] {
							pointers = append(pointers, pkg+"."+ts.Name.Name+"."+name)
						}
					}
				}
				return true
			})
			return nil
		})
	}
	write := func(file string, lines []string) {
		sort.Strings(lines)
		if err := os.WriteFile(filepath.Join(out, file), []byte(strings.Join(lines, "\n")+"\n"), 0o644); err != nil {
			panic(err)
		}
	}
	write("pointer-fields.txt", pointers)
	write("inline-fields.txt", inlines)
	write("renamed-fields.txt", renames)
}
