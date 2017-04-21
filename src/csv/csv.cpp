#include "stdafx.h"
#include "csv.h"
#include "../common/fs.h"
#include "../common/io.h"

/*
	Extract a token from a string based on a given delimiter string
	Returns a pointer to the NULL terminated token (Note: This modifies _Str / _Context)
	If _Str is passed - the context will be initialized to point to _Str
	Otherwise the passed _Context is reused
*/
char* strtok_c(char * _Str, const char * _Delim, char ** _Context)
{
	// Initialize the context if a string was passed
	if (_Str) {
		*_Context = _Str;
	}

	char* ctx = *_Context;
	char* c = strstr(ctx, _Delim);
	if (c != NULL)
	{
		for (const char* d = _Delim; *d; d++)
			*c++ = '\0';
		*_Context = c;
		return ctx;
	}
	
	return NULL;
}

//
// Extract a single entry token from a CSV string
// Automatically handles quotes
//
char* csvtok(char** ctx)
{
	if (**ctx != '"')
		return strtok_c(NULL, ",", ctx);

	char* const tok = *ctx;

	// 'c' is the current char that we are parsing
	// 't' is the target char that we are copying to (for handling escape strings etc.)
	// t < c always
	for (char *c = ++(*ctx), *t = tok; c != '\0'; c++)
	{
		*t++ = *c;
		if (*c == '"')
		{
			// Skip any escaped quotes
			if (*++c == '"')
				continue;

			*ctx = c; // We need to get past the ',' too though
			t[-1] = '\0'; // terminate the token
			break;
		}
	}

	strtok_c(NULL, ",", ctx);
	return tok;
};

CSVStaticTable::CSVStaticTable(void) : buf(NULL)
{
	this->cells.clear();
}

CSVStaticTable::CSVStaticTable(const char* path, int loadflags) : buf(NULL)
{
	this->cells.clear();
	this->ReadFile(path, loadflags);
}

CSVStaticTable::~CSVStaticTable(void)
{
	delete[] buf;
}

const char* CSVStaticTable::FieldName(int field_index) const
{
	_ASSERT(field_index < this->FieldCount());
	return this->cells[0][field_index];
}

const char* CSVStaticTable::CellValue(int row_index, int field_index) const
{
	_ASSERT(row_index < this->RowCount());
	_ASSERT(field_index < this->FieldCount());

	return this->cells[row_index + 1][field_index];
}

int CSVStaticTable::PruneRows(int bits)
{
	if ((bits & (CSV_ST_PRUNE_EMPTY | CSV_ST_PRUNE_COMMENTS)) == 0)
		return 0;

	int pruned_count = 0;
	for (int r = 0; r < this->RowCount();)
	{
		if ((bits & CSV_ST_PRUNE_COMMENTS) && *this->cells[r+1][0] == '#')
		{
			this->cells.erase(this->cells.begin() + r + 1);
			pruned_count++;
		}

		if (bits & CSV_ST_PRUNE_EMPTY)
		{
			bool content = false;
			for (int c = 0; c < this->FieldCount(); c++)
			{
				if (strlen(this->cells[r+1][c]))
				{
					content = true;
					r++;
					break;
				}
			}

			if (!content)
			{
				this->cells.erase(this->cells.begin() + r + 1);
				pruned_count++;
			}
		}
	}

	if (pruned_count)
		Con_Printf_v("Pruned %d empty rows from table\n", pruned_count);

	return pruned_count;
}

void CSVStaticTable::DeleteRow(int row_index)
{
	this->cells.erase(this->cells.begin() + 1 + row_index);
}

int CSVStaticTable::PruneColumns(void)
{
	int pruned_count = 0;

	for (int c = 0, actual_column = 0; c < this->FieldCount(); actual_column++)
	{
		if (*this->FieldName(c) != '\0')
		{
			c++;
			continue;
		}

		for (int r = 1; r < this->RowCount(); r++)
		{
			if (*this->CellValue(r, c) != '\0')
				Con_Warning("Warning: Ignoring value with unnamed field (see field %d)\n", actual_column);

			this->cells[r].erase(this->cells[r].begin() + c);
		}

		this->cells[0].erase(this->cells[0].begin() + c);
		pruned_count++;
	}

	if (pruned_count)
		Con_Printf_v("Pruned %d empty columns from table\n", pruned_count);

	return pruned_count;
}

int CSVStaticTable::ReadFile(const char* path)
{
	return this->ReadFile(path, CSV_ST_DEFAULT);
}

int CSVStaticTable::ReadFile(const char* path, int loadflags)
{
	Con_Printf_v("Loading CSV '%s...'\n", FS_GetFilenameSubString(path));
	int size = FS_FileSize(path);

	this->buf = new char[size + 1];
	memset(buf, 0, size + 1);

	FILE* f;
	fopen_s(&f, path, "rb"); // text mode doesnt like to read the trailing \r\n
	if (!f)
	{
		Con_Error("Unable to open file '%s' for reading\n", path);
		delete[] buf;
		this->buf = NULL;
		return 1;
	}

	int read = fread(buf, 1, size, f);
	fclose(f);

	if (read <= 0)
		return 3;

	char* context = buf;
	char* tok = strtok_c(buf, "\r\n", &context);
	for (int i = 0; tok; i++)
	{
		cells.resize(i+1);
		std::vector<const char*>& row = cells[cells.size() - 1];

		
		char* nt = tok;
		char* tk = csvtok(&nt);

		for (int t = 0; tk; t++)
		{
			row.push_back(tk);
			tk = csvtok(&nt);
		}
		row.push_back(nt);

		if (row.size() != cells[0].size())
		{
			Con_Error("Error: Incorrect number of fields on row %d - found %d, expected %d\n", i, row.size(), cells[0].size());
			this->cells.clear();
			delete[] this->buf;
			this->buf = NULL;
			return 2;
		}

		tok = strtok_c(NULL, "\r\n", &context);
	}

	if (loadflags & CSV_ST_PRUNE_EMPTY)
		this->PruneColumns();
	this->PruneRows(loadflags);

	if (loadflags & CSV_ST_HEADERLESS_SINGLEFIELD)
	{
		std::vector<const char*> fields;
		fields.push_back("name");
		this->cells.insert(cells.begin(), fields);
	}

	return 0;
}

int CSVStaticTable::WriteFile(const char* path, bool overwrite) const
{
	if (FS_FileExists(path) && !overwrite)
	{
		Con_Error("File '%s' already exists\n", path);
		return 1;
	}

	FILE* f;
	fopen_s(&f, path, "w"); // text mode doesnt like to read the trailing \r\n
	if (!f)
	{
		Con_Error("Unable to open file '%s' for writing\n", path);
		return 2;
	}

	this->PrintTable(f, false);

	fclose(f);
	return 0;
}

void CSVStaticTable::PrintTable(FILE* h, bool include_debug_info) const
{
	for (unsigned int r = 0; r < cells.size(); r++)
	{
		if (include_debug_info)
			fprintf(h, "[%d]: ", r);

		for (unsigned int f = 0; f < cells[r].size(); f++)
		{
			const char* str = cells[r][f];
			if (strpbrk(str, ",\"") != NULL)
			{
				fwrite("\"", 1, 1, h);
				for (const char* c = str; *c; c++)
				{
					if (*c == '"')
						fwrite("\"\"", 1, 2, h);
					else
						fwrite(c, 1, 1, h);
				}
				fwrite("\"", 1, 1, h);
			}
			else
			{
				fprintf(h, "%s", str);
			}

			// Write the delimiter character
			// The last entry in a row doesnt have one at the end
			if (f + 1 < cells[r].size())
				fwrite(",", 1, 1, h);
		}
		fprintf(h, "\n");
	}
}
